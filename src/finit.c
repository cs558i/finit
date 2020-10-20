/* Finit - Fast /sbin/init replacement w/ I/O, hook & service plugins
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2020 Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <ctype.h>
#include <dirent.h>
#ifdef HAVE_FSTAB_H
#include <fstab.h>
#endif
#include <mntent.h>
#include <time.h>		/* tzet() */
#include <sys/mount.h>
#include <sys/stat.h>		/* umask(), mkdir() */
#include <sys/wait.h>
#include <lite/lite.h>

#include "finit.h"
#include "cgroup.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "service.h"
#include "sig.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "schedule.h"

int   runlevel  = 0;		/* Bootstrap 'S' */
int   cfglevel  = RUNLEVEL;	/* Fallback if no configured runlevel */
int   prevlevel = -1;
int   rescue    = 0;		/* rescue mode from kernel cmdline */
int   single    = 0;		/* single user mode from kernel cmdline */
int   splash    = 0;		/* splash + progress enabled on kernel cmdline */
char *sdown     = NULL;
char *network   = NULL;
char *hostname  = NULL;
char *rcsd      = FINIT_RCSD;
char *runparts  = NULL;

uev_ctx_t *ctx  = NULL;		/* Main loop context */
svc_t *wdog     = NULL;		/* No watchdog by default */


/*
 * Show user configured banner before service bootstrap progress
 */
static void banner(void)
{
	plugin_run_hooks(HOOK_BANNER);

	if (log_is_silent())
		return;

	print_banner(INIT_HEADING);
}

/*
 * Check all filesystems in /etc/fstab with a fs_passno > 0
 */
static int fsck(int pass)
{
	struct fstab *fs;
	int rc = 0;

	if (!setfsent()) {
		_pe("Failed opening fstab");
		return 1;
	}

	while ((fs = getfsent())) {
		char cmd[80];
		struct stat st;

		if (fs->fs_passno != pass)
			continue;

		errno = 0;
		if (stat(fs->fs_spec, &st) || !S_ISBLK(st.st_mode)) {
			if (!string_match(fs->fs_spec, "UUID=") && !string_match(fs->fs_spec, "LABEL=")) {
				_d("Cannot fsck %s, not a block device: %s", fs->fs_spec, strerror(errno));
				continue;
			}
		}

		if (ismnt("/proc/mounts", fs->fs_file, "rw")) {
			_d("Skipping fsck of %s, already mounted rw on %s.", fs->fs_spec, fs->fs_file);
			continue;
		}

		snprintf(cmd, sizeof(cmd), "fsck -a %s", fs->fs_spec);
		rc += run_interactive(cmd, "Checking filesystem %.13s", fs->fs_spec);
	}

	endfsent();

	return rc;
}

static int fsck_all(void)
{
	int pass, rc = 0;

	for (pass = 1; pass < 10; pass++) {
		rc = fsck(pass);
		if (rc)
			break;
	}

	return rc;
}

#ifndef SYSROOT
static void fs_remount_root(int fsckerr)
{
	struct fstab *fs;

	if (!setfsent())
		return;

	while ((fs = getfsent())) {
		if (!strcmp(fs->fs_file, "/"))
			break;
	}

	/* If / is not listed in fstab, or listed as 'ro', leave it
	 * alone. */
	if (!fs || !strcmp(fs->fs_type, "ro"))
		goto out;

	if (fsckerr)
		print(1, "Cannot remount / as read-write, fsck failed before");
	else
		run_interactive("mount -n -o remount,rw /",
				"Remounting / as read-write");

out:
	endfsent();
}
#else
static void fs_remount_root(int fsckerr)
{
	/*
	 * XXX: Untested, in the initramfs age we should
	 *      probably use switch_root instead.
	 */
	rc = mount(SYSROOT, "/", NULL, MS_MOVE, NULL);
}
#endif	/* SYSROOT */

static void fs_init(void)
{
	int fsckerr;

	if (!rescue) {
		fsckerr = fsck_all();
		fs_remount_root(fsckerr);
	}


	_d("Root FS up, calling hooks ...");
	plugin_run_hooks(HOOK_ROOTFS_UP);

	if (run_interactive("mount -na", "Mounting filesystems"))
		plugin_run_hooks(HOOK_MOUNT_ERROR);

	_d("Calling extra mount hook, after mount -a ...");
	plugin_run_hooks(HOOK_MOUNT_POST);

	run("swapon -ea");
	umask(0022);
}

/*
 * If everything goes south we can use this to give the operator an
 * emergency shell to debug the problem -- Finit should not crash!
 *
 * Note: Only use this for debugging a new Finit setup, don't use
 *       this in production since it gives a root shell to anyone
 *       if Finit crashes.
 *
 * This emergency shell steps in to prevent "Aieee, PID 1 crashed"
 * messages from the kernel, which usually results in a reboot, so
 * that the operator instead can debug the problem.
 */
static void emergency_shell(void)
{
#ifdef EMERGENCY_SHELL
	pid_t pid;

	pid = fork();
	if (pid) {
		while (1) {
			pid_t id;

			/* Reap 'em (prevents Zombies) */
			id = waitpid(-1, NULL, WNOHANG);
			if (id == pid)
				break;
		}

		fprintf(stderr, "\n=> Embarrassingly, Finit has crashed.  Check /dev/kmsg for details.\n");
		fprintf(stderr,   "=> To debug, add 'debug' to the kernel command line.\n\n");

		/*
		 * Become session leader and set controlling TTY
		 * to enable Ctrl-C and job control in shell.
		 */
		setsid();
		ioctl(STDIN_FILENO, TIOCSCTTY, 1);

		execl(_PATH_BSHELL, _PATH_BSHELL, NULL);
	}
#endif /* EMERGENCY_SHELL */
}

/*
 * Handle bootstrap transition to configured runlevel, start TTYs
 *
 * This is the final stage of bootstrap.  It changes to the default
 * (configured) runlevel, calls all external start scripts and final
 * bootstrap hooks before bringing up TTYs.
 *
 * We must ensure that all declared `task [S]` and `run [S]` jobs in
 * finit.conf, or *.conf in finit.d/, run to completion before we
 * finalize the bootstrap process by calling this function.
 */
static void finalize(void)
{
	/*
	 * Run startup scripts in the runparts directory, if any.
	 */
	if (runparts && fisdir(runparts) && !rescue)
		run_parts(runparts, NULL);

	/*
	 * Start all tasks/services in the configured runlevel
	 */
	_d("Change to default runlevel, start all services ...");
	service_runlevel(cfglevel);

	/* Clean up bootstrap-only tasks/services that never started */
	_d("Clean up all bootstrap-only tasks/services ...");
	svc_prune_bootstrap();

	/* All services/tasks/inetd/etc. in configure runlevel have started */
	_d("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Convenient SysV compat for when you just don't care ... */
	if (!access(FINIT_RC_LOCAL, X_OK) && !rescue)
		run_interactive(FINIT_RC_LOCAL, "Calling %s", FINIT_RC_LOCAL);

	/* Hooks that should run at the very end */
	_d("Calling all system up hooks ...");
	plugin_run_hooks(HOOK_SYSTEM_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Enable silent mode before starting TTYs */
	_d("Going silent ...");
	log_silent();

	/* Delayed start of TTYs at bootstrap */
	_d("Launching all getty services ...");
	tty_runlevel();
}

/*
 * Start cranking the big state machine
 */
static void crank_worker(void *unused)
{
	/*
	 * Initalize state machine and start all bootstrap tasks
	 * NOTE: no network available!
	 */
	sm_init(&sm);
	sm_step(&sm);
}

/*
 * Wait for system bootstrap to complete, all SVC_TYPE_RUNTASK must be
 * allowed to complete their work in [S], or timeout, before we call
 * finalize(), should not take more than 120 sec.
 */
static void final_worker(void *work)
{
	static int cnt = 120;

	_d("Step all services ...");
	service_step_all(SVC_TYPE_ANY);

	if (cnt-- > 0 && !service_completed()) {
		_d("Not all bootstrap run/tasks have completed yet ... %d", cnt);
		schedule_work(work);
		return;
	}

	if (cnt > 0)
		_d("All run/task have completed, resuming bootstrap.");
	else
		_d("Timeout, resuming bootstrap.");

	finalize();
}

int main(int argc, char *argv[])
{
	struct wq crank = {
		.cb = crank_worker
	};
	struct wq final = {
		.cb = final_worker,
		.delay = 1000
	};
	uev_ctx_t loop;

	/*
	 * finit/init/telinit client tool uses /dev/initctl pipe
	 * for compatibility but initctl client tool uses socket
	 */
	if (getpid() != 1)
		return client(argc, argv);

	/*
	 * Parse kernel command line (debug, rescue, splash, etc.)
	 * Also calls log_init() to set correct log level
	 */
	conf_parse_cmdline(argc, argv);

	/*
	 * Initalize event context.
	 */
	uev_init1(&loop, 1);
	ctx = &loop;

	/*
	 * Set PATH, SHELL, PWD, and umask early to something sane
	 */
	setenv("PATH", _PATH_STDPATH, 1);
	setenv("SHELL", _PATH_BSHELL, 1);

	chdir("/");
	umask(0);

	/* Set up canvas */
	if (!rescue && !log_is_debug())
		screen_init();

	/*
	 * In case of emergency.
	 */
	emergency_shell();

	/* Load plugins early, the first hook is in banner(), so we
	 * need plugins loaded before calling it. */
	plugin_init(&loop);

	/*
	 * Hello world.
	 */
	banner();

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Initialize default control groups, if available
	 */
	cgroup_init();


	/* Check and mount filesystems. */
	fs_init();

	/*
	 * Initialize .conf system and load static /etc/finit.conf.
	 */
	conf_init();

	/* Bootstrap conditions, needed for hooks */
	cond_init();

	/* Emit conditions for early hooks that ran before the
	 * condition system was initialized in case anyone . */
	cond_set_oneshot(plugin_hook_str(HOOK_BANNER));
	cond_set_oneshot(plugin_hook_str(HOOK_ROOTFS_UP));

	/* Base FS up, enable standard SysV init signals */
	sig_setup(&loop);

	_d("Base FS up, calling hooks ...");
	plugin_run_hooks(HOOK_BASEFS_UP);

	/*
	 * Set up inotify watcher for /etc/finit.d and read all .conf
	 * files to figure out how to bootstrap the system.
	 */
	conf_monitor(&loop);

	_d("Starting initctl API responder ...");
	api_init(&loop);
	umask(022);

	_d("Starting the big state machine ...");
	schedule_work(&crank);

	_d("Starting bootstrap finalize timer ...");
	schedule_work(&final);

	/*
	 * Enter main loop to monitor /dev/initctl and services
	 */
	_d("Entering main loop ...");
	return uev_run(&loop, 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
