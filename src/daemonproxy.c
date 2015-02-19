/* daemonproxy.c - main routines of daemonproxy
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

bool     main_terminate= false;
int      main_exitcode= 0;
controller_t *interactive_controller;

wake_t   main_wake; // global used for tracking things that should wake the main loop
wake_t  *wake= &main_wake; // this is exported to other modules

void wake_at_time(int64_t ts) {
	if (wake->next - ts > 0)
		wake->next= ts;
}

const char * copyright=
	"Copyright (C) 2014-2015  Michael Conrad";
const char * license=
	"Distributed under GPLv2.  See LICENSE.\n"
	"This is free software: you are free to change and redistribute it.\n"
	"There is NO WARRANTY, to the extent permitted by law.\n";


// Parse options and apply to global vars; calls fatal() on failure.
bool set_exec_on_exit(strseg_t arguments_tsv);
static bool register_open_fds();
static bool setup_interactive_mode();
static bool setup_config_file(const char *path);
static void fd_to_dev_null(fd_t *fd);
static void daemonize();

int main(int argc, char** argv) {
	int wstat, ret;
	pid_t pid;
	service_t *svc;
	memset(wake, 0, sizeof(*wake));
	
	log_init();
	svc_init();
	fd_init();
	ctl_init();

	// Special defaults when running as init
	if (getpid() == 1) {
		opt_config_file= CONFIG_FILE_DEFAULT_PATH;
		opt_terminate_guard= 1;
	}
	
	umask(077);

	// parse arguments, overriding default values
	parse_opts(argv+1);
	
	// Check for required options
	if (!opt_interactive && !opt_config_file && !opt_socket_path)
		fatal(EXIT_BAD_OPTIONS, "require -i or -c or -S");
	
	// Initialize file descriptor object pool
	if (opt_fd_pool_count > 0 && opt_fd_pool_size_each > 0)
		if (!fd_preallocate(opt_fd_pool_count, opt_fd_pool_size_each))
			fatal(EXIT_INVALID_ENVIRONMENT, "Unable to preallocate file descriptor objects");
	if (!fd_init_special_handles())
		fatal(EXIT_BROKEN_PROGRAM_STATE, "Can't initialize all special handles");

	if (!register_open_fds())
		fatal(EXIT_BAD_OPTIONS, "Not enough FD objects to register all open FDs");

	// Set up signal handlers and signal mask and signal self-pipe
	// Do this AFTER registering all open FDs, because it creates a pipe
	sig_init();
	
	// Initialize service object pool
	if (opt_svc_pool_count > 0 && opt_svc_pool_size_each > 0)
		if (!svc_preallocate(opt_svc_pool_count, opt_svc_pool_size_each))
			fatal(EXIT_INVALID_ENVIRONMENT, "Unable to preallocate service objects");

	// Initialize controller object pool
	control_socket_init();

	if (opt_socket_path && !control_socket_start(STRSEG(opt_socket_path)))
		fatal(EXIT_INVALID_ENVIRONMENT, "Can't create controller socket");
	
	if (opt_interactive)
		if (!setup_interactive_mode())
			fatal(EXIT_INVALID_ENVIRONMENT, "stdin/stdout are not usable!");

	if (opt_config_file)
		if (!setup_config_file(opt_config_file))
			fatal(EXIT_INVALID_ENVIRONMENT, "Unable to process config file");

	if (opt_mlockall) {
		// Lock all memory into ram. init should never be "swapped out".
		if (mlockall(MCL_CURRENT | MCL_FUTURE))
			log_error("mlockall: %s", strerror(errno));
	}
	
	// fork and setsid if requested, but not if PID 1 or interactive
	if (opt_daemonize) {
		if (getpid() == 1 || opt_interactive)
			log_warn("Ignoring --daemonize (see manual)");
		else
			daemonize();
	}

	// terminate is disabled when running as init, so this is an infinite loop
	// (except when debugging)
	wake->now= gettime_mon_frac();
	while (!main_terminate) {
		// set our wait parameters so other methods can inject new wake reasons
		wake->next= wake->now + (200LL<<32); // wake at least every 200 seconds
		
		// collect new signals since last iteration and set read-wake on signal fd
		sig_run(wake);
		
		// reap all zombies, possibly waking services
		while ((pid= waitpid(-1, &wstat, WNOHANG)) > 0) {
			log_trace("waitpid found pid = %d", (int)pid);
			if ((svc= svc_by_pid(pid)))
				svc_handle_reaped(svc, wstat);
			else
				log_trace("pid does not belong to any service");
		}
		if (pid < 0)
			log_trace("waitpid: %s", strerror(errno));
		
		// run state machine of each service that is active.
		svc_run_active(wake);
		
		// possibly accept new controller connections
		control_socket_run();
		
		// run controller state machines
		ctl_run(wake);
		
		log_run();
		
		// Wait until an event or the next time a state machine needs to run
		// (state machines edit wake.next)
		wake->now= gettime_mon_frac();
		int delay= 0;
		if (wake->next - wake->now > 0) {
			delay= (int)(((wake->next - wake->now) >> 10) * 1000 >> 22);
			log_trace("wait up to %d msec", delay);
		}
		
		ret= poll(wake->poll_slots, sizeof(wake->poll_slots)/sizeof(*wake->poll_slots), delay);
		if (ret < 0) {
			// shouldn't ever fail, but if not EINTR, at least log it and prevent
			// looping too fast
			if (errno != EINTR) {
				log_error("poll: %s", strerror(errno));
				usleep(500000);
			}
		}
		wake->now= gettime_mon_frac();
	}
	
	if (opt_exec_on_exit)
		fatal(main_exitcode, "terminated normally");

	log_info("daemonproxy exiting");
	log_running_services();
	return main_exitcode;
}

static bool setup_interactive_mode() {
	controller_t *ctl;
	fd_t *stdin_fd= fd_by_name(STRSEG("stdin"));
	fd_t *stdout_fd= fd_by_name(STRSEG("stdout"));

	if (!stdin_fd || !stdout_fd) {
		log_error("stdin/stdout not available");
		return false;
	}

	if (!(ctl= ctl_new(0, 1))) {
		log_error("Failed to allocate controller");
		return false;
	}

	// if command stream is interrupted, do not execute the final command
	interactive_controller= ctl;
	ctl_set_auto_final_newline(ctl, false);

	// stdin is used now, so make the named "stdin" handle a dup of /dev/null
	// if dup() fails, they become -1, which is the desired fallback.
	
	fd_to_dev_null(stdin_fd);
	fd_to_dev_null(stdout_fd);
	
	// ctl_write is not guaranteed to finish without blocking, but in practice
	// this should all fit into the pipe, and it isn't critical anyway.
	
	ctl_write(ctl, "info\tdaemonproxy version %d.%d.%d%s (git %.8s%s)\n",
		version_major, version_minor, version_release, version_suffix, version_git_head, version_git_dirty? "+":"");
	ctl_write(ctl, "info\t%s\n", copyright);
	strseg_t text= STRSEG(license), line;
	while (strseg_tok_next(&text, '\n', &line) && line.len > 0)
		ctl_write(ctl, "info\t%.*s\n", line.len, line.data);
	ctl_write(ctl, "info\tInteractive mode.  Use ^D or 'exit' to terminate.  See 'man daemonproxy' for command syntax.\n");
	
	return true;
}

void main_notify_controller_freed(controller_t *ctl) {
	if (interactive_controller && ctl == interactive_controller) {
		// treat this as an exit request
		if (!opt_terminate_guard) {
			log_info("interactive session ended");
			main_terminate= true;
			wake->next= wake->now;
		}
		else {
			log_warn("interactive session ended, but not exiting due to terminate-guard");
		}
		interactive_controller= NULL;
	}
}

static void daemonize() {
	fd_t *fd;
	int fdnum, i;
	pid_t pid;

	if ((pid= fork()) < 0)
		fatal(EXIT_INVALID_ENVIRONMENT, "fork: %s", strerror(errno));
	// The parent writes PID to stdout, and exits immediately
	else if (pid > 0) {
		// print PID of daemon on stdout
		printf("%d", (int) pid);
		fflush(NULL);
		// do not run any cleanup
		_exit(0);
	}
	// The child closes all standard file handles, and becomes session leader.
	// We don't need to do an additional fork (which prevents acquiring a controlling tty)
	// because all further calls to 'open' are passed the "NOCTTY" flag.
	else {
		// For stdin/stdout/stderr, close the handle if it was originally open
		// and hasn't been claimed by something like config file on stdin.
		// Then make the named FD a dup of /dev/null
		// (claimed descriptors will be dup'd copies of /dev/null already)
		for (i= 0; i < 3; i++) {
			fd= fd_by_name(i == 0? STRSEG("stdin") : i == 1? STRSEG("stdout") : STRSEG("stderr"));
			log_trace("fd %d %s fdnum is %d", i, fd? fd_get_name(fd) : "null", fd? fd_get_fdnum(fd) : -1);
			if (fd && (fdnum= fd_get_fdnum(fd)) == i) {
				fd_to_dev_null(fd);
				close(i);
			}
		}
		// and become our own session and process group
		if (setsid() == -1)
			fatal(EXIT_INVALID_ENVIRONMENT, "setsid: %s", strerror(errno));
	}
}

// Add all open file descriptors as named FD objects
static bool register_open_fds() {
	int i, fdnum;
	bool result= true, is_open;
	char buffer[16];

	for (i= 0; i < 1024; i++) {
		is_open= i != fd_dev_null && fcntl(i, F_GETFL) != -1;
		// for stdin,stdout,stderr, we create it as a dup of dev_null if it isn't open.
		// for all others, we only create it if it is open.
		if (is_open || i < 3) {
			if (i < 3)
				strcpy(buffer, i == 0? "stdin" : i == 1? "stdout" : "stderr");
			else
				snprintf(buffer, sizeof(buffer), "fd_%d", i);

			fdnum= is_open? i : dup(fd_dev_null);
			log_trace("registering %s as %d", buffer, fdnum);
			result= fd_new_unknown(STRSEG(buffer), fdnum)
				&& result;
		}
	}
	return result;
}

static void fd_to_dev_null(fd_t *fd) {
	int fdnum= dup(fd_dev_null);
	log_trace("reassigning %s to %d", fd_get_name(fd), fdnum);
	fd_set_fdnum(fd, fdnum);
}

static bool setup_config_file(const char *path) {
	fd_t *stdin_fd= NULL;
	controller_t *ctl;
	int f;
	
	if (0 == strcmp(path, "-")) {
		stdin_fd= fd_by_name(STRSEG("stdin"));
		if (!stdin_fd || fd_get_fdnum(stdin_fd) != 0) {
			log_error("stdin not available");
			return false;
		}
		f= 0;
	}
	else {
		f= open(path, O_RDONLY|O_NONBLOCK|O_NOCTTY);
		if (f == -1) {
			log_error("Failed to open config file \"%s\": %s",
				path, strerror(errno));
			return false;
		}
	}
	
	if (!(ctl= ctl_new(f, -1))) {
		if (f != 0) close(f);
		log_error("Failed to allocate controller");
		return false;
	}
	
	ctl_set_auto_final_newline(ctl, true);
	// stdin is used, so make the named "stdin" handle a dup of /dev/null
	if (stdin_fd)
		fd_to_dev_null(stdin_fd);
	return true;
}

// returns monotonic time as a 32.32 fixed-point number 
int64_t gettime_mon_frac() {
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
		t.tv_sec= time(NULL);
		t.tv_nsec= 0;
	}
	return (int64_t) (
		(((uint64_t) t.tv_sec) << 32)
		| (((((uint64_t) t.tv_nsec)<<32) / 1000000000 ) & 0xFFFFFFFF)
	);
}

/** Exit (or not) from a fatal condition
 *
 * If exec-on-exit is set, this will exec into (what we expect to be) the
 * cleanup handler.
 *
 * Else if failsafe is enabled, we do not exit, and attempt to continue.
 *
 * Else we exit with the exitcode passed to us.
 *
 * Note that the final log message (or more) might be lost because STDERR
 * is non-blocking.
 */
void fatal(int exitcode, const char *msg, ...) {
	char msgbuf[1024], numbuf[12];
	int i;
	va_list val;
	strseg_t args, arg;
	char **argv;

	if (msg && msg[0]) {
		va_start(val, msg);
		vsnprintf(msgbuf, sizeof(msgbuf), msg, val);
		va_end(val);
	}
	else msgbuf[0]= '\0';

	if (opt_exec_on_exit) {
		// Pass params to child as environment vars
		snprintf(numbuf, sizeof(numbuf), "%d", exitcode);
		setenv("DAEMONPROXY_EXITCODE", numbuf, 1);
		setenv("DAEMONPROXY_ERROR", msgbuf, 1);
		// count argument list
		args= opt_exec_on_exit_args;
		for (i= 0; strseg_tok_next(&args, '\0', NULL); i++);
		log_debug("%d arguments to exec", i);
		// build argv
		argv= alloca(sizeof(char*) * (i+1));
		args= opt_exec_on_exit_args;
		for (i= 0; strseg_tok_next(&args, '\0', &arg); i++)
			argv[i]= (char*) arg.data;
		argv[i]= NULL; // required by spec
		// exec child
		sig_reset_for_exec();
		log_warn("daemonproxy exec_on_exit to '%s'", argv[0]);
		log_running_services();
		execvp(argv[0], argv);
		// If that failed... continue?  we might be screwed here.
		sig_init();
		log_error("Unable to exec \"%s\": %s", argv[0], strerror(errno));
	}

	if (msgbuf[0])
		log_write(LOG_LEVEL_FATAL, "%s%s", opt_terminate_guard? "(attempting to continue) ":"", msgbuf);

	if (!opt_terminate_guard) {
		log_running_services();
		exit(exitcode);
	}
}
