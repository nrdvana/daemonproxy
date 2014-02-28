/* daemonproxy.c - main routines of daemonproxy
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

bool     main_terminate= false;
int      main_exitcode= 0;

wake_t main_wake;

wake_t *wake= &main_wake;

// Parse options and apply to global vars; calls fatal() on failure.
bool create_standard_handles(int dev_null);
bool set_exec_on_exit(strseg_t arguments_tsv);
static void daemonize();

int main(int argc, char** argv) {
	int wstat, f, ret;
	pid_t pid;
	struct timeval tv;
	bool config_on_stdin= false;
	service_t *svc;
	controller_t *ctl;
	wake_t wake_instance;
	
	memset(&wake_instance, 0, sizeof(wake_instance));
	wake= &wake_instance;
	
	// Special defaults when running as init
	if (getpid() == 1) {
		opt_config_file= CONFIG_FILE_DEFAULT_PATH;
		opt_terminate_guard= 1;
	}
	
	umask(077);

	log_init();
	
	// parse arguments, overriding default values
	parse_opts(argv+1);
	
	// Check for required options
	if (!opt_interactive && !opt_config_file && !opt_socket_path)
		fatal(EXIT_BAD_OPTIONS, "require -i or -c or -S");
	
	// Set up signal handlers and signal mask and signal self-pipe
	sig_init();
	
	// Initialize file descriptor object pool and indexes
	fd_init();
	if (opt_fd_pool_count > 0 && opt_fd_pool_size_each > 0)
		if (!fd_preallocate(opt_fd_pool_count, opt_fd_pool_size_each))
			fatal(EXIT_INVALID_ENVIRONMENT, "Unable to preallocate file descriptor objects");

	// A handle to dev/null is mandatory...
	f= open("/dev/null", O_RDWR|O_NOCTTY);
	log_trace("open(/dev/null) => %d", f);
	if (f < 0) {
		fatal(EXIT_INVALID_ENVIRONMENT, "Can't open /dev/null: %s (%d)", strerror(errno), errno);
		// if we're in failsafe mode, fatal doesn't exit()
		log_error("Services using 'null' descriptor will get closed handles instead!");
		f= -1;
	}
	if (!create_standard_handles(f))
		fatal(EXIT_INVALID_ENVIRONMENT, "Can't allocate standard handle objects");

	// Initialize service object pool and indexes
	svc_init();
	if (opt_svc_pool_count > 0 && opt_svc_pool_size_each > 0)
		if (!svc_preallocate(opt_svc_pool_count, opt_svc_pool_size_each))
			fatal(EXIT_INVALID_ENVIRONMENT, "Unable to preallocate service objects");

	// Initialize controller object pool
	ctl_init();
	control_socket_init();

	if (opt_socket_path && !control_socket_start(STRSEG(opt_socket_path)))
		fatal(EXIT_INVALID_ENVIRONMENT, "Can't create controller socket");
	
	if (opt_config_file) {
		if (0 == strcmp(opt_config_file, "-"))
			config_on_stdin= true;
		else {
			f= open(opt_config_file, O_RDONLY|O_NONBLOCK|O_NOCTTY);
			if (f == -1)
				fatal(EXIT_INVALID_ENVIRONMENT, "failed to open config file \"%s\": %s (%d)",
					opt_config_file, strerror(errno), errno);
			else if (!(ctl= ctl_new(f, -1))) {
				close(f);
				fatal(EXIT_BROKEN_PROGRAM_STATE, "failed to allocate controller for config file!");
			}
			else
				ctl_set_auto_final_newline(ctl, true);
		}
	}
	
	if (opt_interactive || config_on_stdin) {
		if (!(ctl= ctl_new(0, 1)))
			fatal(EXIT_BROKEN_PROGRAM_STATE, "failed to initialize stdio controller client!");
		else
			ctl_set_auto_final_newline(ctl, config_on_stdin);
	}
	
	if (opt_mlockall) {
		// Lock all memory into ram. init should never be "swapped out".
		if (mlockall(MCL_CURRENT | MCL_FUTURE))
			log_error("mlockall: %s", strerror(errno));
	}
	
	// fork and setsid if requested, but not if PID 1 or interactive
	if (opt_daemonize) {
		if (getpid() == 1 || opt_interactive || config_on_stdin)
			log_warn("Ignoring --daemonize (see manual)");
		else
			daemonize();
	}

	// terminate is disabled when running as init, so this is an infinite loop
	// (except when debugging)
	wake->now= gettime_mon_frac();
	FD_ZERO(&wake->fd_read);
	FD_ZERO(&wake->fd_write);
	FD_ZERO(&wake->fd_err);
	while (!main_terminate) {
		// set our wait parameters so other methods can inject new wake reasons
		wake->next= wake->now + (200LL<<32); // wake at least every 200 seconds
		wake->max_fd= -1;
		
		sig_enable(false);
		
		// report signals and set read-wake on signal fd
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
		
		sig_enable(true);

		// Wait until an event or the next time a state machine needs to run
		// (state machines edit wake.next)
		wake->now= gettime_mon_frac();
		if (wake->next - wake->now > 0) {
			tv.tv_sec= (long)((wake->next - wake->now) >> 32);
			tv.tv_usec= (long)((((wake->next - wake->now)&0xFFFFFFFFLL) * 1000000) >> 32);
			log_trace("wait up to %d.%d sec", tv.tv_sec, tv.tv_usec);
		}
		else
			tv.tv_sec= tv.tv_usec= 0;
		
		ret= select(wake->max_fd+1, &wake->fd_read, &wake->fd_write, &wake->fd_err, &tv);
		if (ret < 0) {
			// shouldn't ever fail, but if not EINTR, at least log it and prevent
			// looping too fast
			if (errno != EINTR) {
				log_error("select: %s", strerror(errno));
				tv.tv_usec= 500000;
				tv.tv_sec= 0;
				select(0, NULL, NULL, NULL, &tv); // use it as a sleep, this time
			}
		}
		wake->now= gettime_mon_frac();
	}
	
	if (opt_exec_on_exit)
		fatal(main_exitcode, "terminated normally");
	return main_exitcode;
}

void daemonize() {
	pid_t pid= fork();
	if (pid < 0)
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
		close(0);
		close(1);
		close(2);
		if (setsid() == -1)
			fatal(EXIT_INVALID_ENVIRONMENT, "setsid: %s", strerror(errno));
	}
}

bool create_standard_handles(int dev_null) {
	return fd_new_file(STRSEG("null"), dev_null,
				(fd_flags_t){ .special= true, .read= true, .write= true, .is_const= true },
				STRSEG("/dev/null"))

		&& fd_new_file(STRSEG("stdin"), 0,
				(fd_flags_t){ .special= true, .read= true, .write= false, .is_const= true },
				STRSEG("standard-in of daemonproxy"))
		
		&& fd_new_file(STRSEG("stdout"), 1,
				(fd_flags_t){ .special= true, .read= false, .write= true, .is_const= true },
				STRSEG("standard-out of daemonproxy"))
		
		&& fd_new_file(STRSEG("stderr"), 2,
				(fd_flags_t){ .special= true, .read= false, .write= true, .is_const= true },
				STRSEG("standard-err of daemonproxy"))
		
		&& fd_new_file(STRSEG("control.event"), -2,
				(fd_flags_t){ .special= true, .read= true, .write= false, .is_const= true },
				STRSEG("daemonproxy event stream"))

		&& fd_new_file(STRSEG("control.cmd"), -3,
				(fd_flags_t){ .special= true, .read= false, .write= true, .is_const= true },
				STRSEG("daemonproxy commanbd pipe"))
	;
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
	char buffer[1024];
	int i;
	va_list val;
	strseg_t args, arg;
	char **argv;
	
	if (msg && msg[0]) {
		va_start(val, msg);
		vsnprintf(buffer, sizeof(buffer), msg, val);
		va_end(val);
	} else {
		buffer[0]= '\0';
	}
	
	if (opt_exec_on_exit) {
		// Pass params to child as environment vars
		setenv("INIT_FRAME_ERROR", buffer, 1);
		sprintf(buffer, "%d", exitcode);
		setenv("INIT_FRAME_EXITCODE", buffer, 1);
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
		execvp(argv[0], argv);
		// If that failed... continue?  we might be screwed here.
		sig_init();
		log_error("Unable to exec \"%s\": %s", argv[0], strerror(errno));
	}
	
	if (buffer[0])
		log_write(LOG_LEVEL_FATAL, "%s%s", opt_terminate_guard? "(attempting to continue) ":"", buffer);
	if (!opt_terminate_guard)
		exit(exitcode);
}
