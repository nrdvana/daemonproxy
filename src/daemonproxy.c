/* daemonproxy.c - main routines of daemonproxy
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

bool     main_terminate= false;
int      main_exitcode= 0;
int64_t  main_terminate_guard= 0;
const char *main_cfgfile= NULL;
bool     main_exec_on_exit= false;
char     main_exec_on_exit_buf[256];
strseg_t main_exec_on_exit_args;
bool     main_use_stdin= false;
bool     main_mlockall= false;

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
		main_cfgfile= CONFIG_FILE_DEFAULT_PATH;
		main_terminate_guard= 1;
	}
	
	log_init();
	
	// parse arguments, overriding default values
	parse_opts(argv+1);
	
	// Check for required options
	if (!main_use_stdin && !main_cfgfile)
		fatal(EXIT_BAD_OPTIONS, "require --stdin or -c");
	
	// fork and setsid if requested, but not if PID 1 or interactive
	if (opt_daemonize) {
		if (getpid() == 1 || main_use_stdin)
			log_warn("Ignoring --daemonize (see manual)");
		else
			daemonize();
	}
	// Set up signal handlers and signal mask and signal self-pipe
	sig_init();
	
	// Initialize file descriptor object pool and indexes
	fd_init();
	if (opt_fd_pool_count > 0 && opt_fd_pool_size_each > 0)
		if (!fd_preallocate(opt_fd_pool_count, opt_fd_pool_size_each))
			fatal(EXIT_INVALID_ENVIRONMENT, "Unable to preallocate file descriptor objects");

	// A handle to dev/null is mandatory...
	f= open("/dev/null", O_RDWR);
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
	
	if (main_cfgfile) {
		if (0 == strcmp(main_cfgfile, "-"))
			config_on_stdin= true;
		else {
			f= open(main_cfgfile, O_RDONLY|O_NONBLOCK|O_NOCTTY);
			if (f == -1)
				fatal(EXIT_INVALID_ENVIRONMENT, "failed to open config file \"%s\": %s (%d)",
					main_cfgfile, strerror(errno), errno);
			else if (!(ctl= ctl_new(f, -1))) {
				close(f);
				fatal(EXIT_BROKEN_PROGRAM_STATE, "failed to allocate controller for config file!");
			}
			else
				ctl_set_auto_final_newline(ctl, true);
		}
	}
	
	if (main_use_stdin || config_on_stdin) {
		if (!(ctl= ctl_new(0, 1)))
			fatal(EXIT_BROKEN_PROGRAM_STATE, "failed to initialize stdio controller client!");
		else
			ctl_set_auto_final_newline(ctl, config_on_stdin);
	}
	
	if (main_mlockall) {
		// Lock all memory into ram. init should never be "swapped out".
		if (mlockall(MCL_CURRENT | MCL_FUTURE))
			perror("mlockall");
	}
	
	// terminate is disabled when running as init, so this is an infinite loop
	// (except when debugging)
	wake->now= gettime_mon_frac();
	while (!main_terminate) {
		// set our wait parameters so other methods can inject new wake reasons
		wake->next= wake->now + (200LL<<32); // wake at least every 200 seconds
		wake->max_fd= -1;
		FD_ZERO(&wake->fd_read);
		FD_ZERO(&wake->fd_write);
		FD_ZERO(&wake->fd_err);
		
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
		
		// run controller state machines
		ctl_run(wake);
		
		log_flush();
		
		sig_enable(true);

		// Wait until an event or the next time a state machine needs to run
		// (state machines edit wake.next)
		// If we don't need to wait, don't.  (we don't care about select() return code)
		wake->now= gettime_mon_frac();
		if (wake->next - wake->now > 0) {
			tv.tv_sec= (long)((wake->next - wake->now) >> 32);
			tv.tv_usec= (long)((((wake->next - wake->now)&0xFFFFFFFFLL) * 1000000) >> 32);
			log_trace("wait up to %d.%d sec", tv.tv_sec, tv.tv_usec);
			
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
		else log_trace("no wait at main loop");
	}
	
	if (main_exec_on_exit)
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

bool set_exec_on_exit(strseg_t args) {
	int i;
	
	// empty string disables the feature
	if (args.len <= 0) {
		main_exec_on_exit= false;
		return true;
	}
	
	// Stored in a fixed-size buffer...
	if (args.len >= sizeof(main_exec_on_exit_buf))
		return false;

	memcpy(main_exec_on_exit_buf, args.data, args.len);
	main_exec_on_exit_buf[args.len]= '\0';
	
	// convert tab-delimited arguments to NUL-delimited
	for (i= 0; i < args.len; i++)
		if (main_exec_on_exit_buf[i] == '\t')
			main_exec_on_exit_buf[i]= '\0';
	
	main_exec_on_exit= true;
	main_exec_on_exit_args= (strseg_t){ main_exec_on_exit_buf, args.len };
	return true;
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
	
	if (main_exec_on_exit) {
		// Pass params to child as environment vars
		setenv("INIT_FRAME_ERROR", buffer, 1);
		sprintf(buffer, "%d", exitcode);
		setenv("INIT_FRAME_EXITCODE", buffer, 1);
		// count argument list
		args= main_exec_on_exit_args;
		for (i= 0; strseg_tok_next(&args, '\0', NULL); i++);
		log_debug("%d arguments to exec", i);
		// build argv
		argv= alloca(sizeof(char*) * (i+1));
		args= main_exec_on_exit_args;
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
		log_write(LOG_LEVEL_FATAL, "%s%s", main_terminate_guard? "(attempting to continue) ":"", buffer);
	if (!main_terminate_guard)
		exit(exitcode);
}
