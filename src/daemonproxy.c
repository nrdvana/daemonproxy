#include "config.h"
#include "daemonproxy.h"

bool main_terminate= false;
int  main_exitcode= 0;
const char *main_cfgfile= NULL;
bool main_exec_on_exit= false;
char main_exec_on_exit_buf[256];
strseg_t main_exec_on_exit_args;
bool main_use_stdin= false;
bool main_mlockall= false;
int  main_fd_pool_count= -1;
int  main_fd_pool_size_each= -1;
bool main_failsafe= false;
int  main_failsafe_guard_code= 1;

wake_t main_wake;

wake_t *wake= &main_wake;

// Parse options and apply to global vars; return false if fails
void parse_opts(char **argv);
void parse_option(char shortname, char* longname, char ***argv);
int parse_size(const char *str, char **endp);
bool create_standard_handles(int dev_null);

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
		main_failsafe= true;
	}
	
	log_init();
	
	// parse arguments, overriding default values
	parse_opts(argv+1);
	
	// Set up signal handlers and signal mask and signal self-pipe
	sig_init();
	// Initialize file descriptor object pool and indexes
	fd_init();
	if (main_fd_pool_count > 0 && main_fd_pool_size_each > 0)
		if (!fd_preallocate(main_fd_pool_count, main_fd_pool_size_each))
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
	svc_init(SERVICE_POOL_SIZE, SERVICE_OBJ_SIZE);
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
			fatal(3, "failed to initialize stdio controller client!");
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

void parse_opts(char **argv) {
	char *current;
	
	while ((current= *argv++)) {
		if (current[0] == '-') {
			if (current[1] == '-')
				parse_option(0, current+2, &argv);
			else for (++current; *current; current++)
				parse_option(*current, NULL, &argv);
		}
		else
			// if failsafe, will not exit()
			fatal(EXIT_BAD_OPTIONS, "Unexpected argument \"%s\"\n", current);
	}
}

void show_help(char **argv);

void show_version(char **argv);

void set_opt_verbose(char** argv)     { log_set_filter(log_filter-1); }

void set_opt_quiet(char** argv)       { log_set_filter(log_filter+1); }

void set_opt_stdin(char **argv)       { main_use_stdin= true; }

void set_opt_mlockall(char **argv)    { main_mlockall= true;  }

void set_opt_failsafe(char **argv)    { main_failsafe= true;  }

void set_opt_configfile(char** argv ) {
	struct stat st;
	if (stat(argv[0], &st))
		fatal(EXIT_BAD_OPTIONS, "Cannot stat configfile \"%s\"", argv[0]);
	main_cfgfile= argv[0];
}

void set_opt_exec_on_exit(char **argv) {
	struct stat st;
	int i, len= strlen(argv[0]);
	
	if (len >= sizeof(main_exec_on_exit_buf))
		fatal(EXIT_BAD_OPTIONS, "exec-on-exit arguments exceed buffer size");
	// convert tab-delimited arguments to NUL-delimited
	for (i= 0; i < len; i++)
		if (argv[0][i] == '\t')
			argv[0][i]= '\0';
	if (stat(argv[0], &st))
		fatal(EXIT_BAD_OPTIONS, "Cannot stat exec-on-exit program \"%s\"", argv[0]);
	main_exec_on_exit= true;
	memcpy(main_exec_on_exit_buf, argv[0], len+1);
	main_exec_on_exit_args.data= main_exec_on_exit_buf;
	main_exec_on_exit_args.len= len;
}

void set_opt_fd_prealloc(char **argv) {
	int n, m= FD_OBJ_SIZE;
	char *end= NULL;
	n= strtol(argv[0], &end, 10);
	
	if (*end == 'x')
		m= parse_size(end+1, &end);
	if (*end)
		fatal(EXIT_BAD_OPTIONS, "Expected 'N' or 'NxM' where N and M are integers");
	
	if (n < FD_POOL_SIZE_MIN) {
		log_warn("At least %d fd objects required; using minimum", FD_POOL_SIZE_MIN);
		n= FD_POOL_SIZE_MIN;
	} else if (n > FD_POOL_SIZE_MAX) {
		log_warn("fd pool size exceeds max number of allowed file descriptors; limiting to %d", FD_POOL_SIZE_MAX);
		n= FD_POOL_SIZE_MAX;
	}
	
	if (m < min_fd_obj_size) {
		log_warn("fd obj size increased to minimum of %d", min_fd_obj_size);
		m= min_fd_obj_size;
	}
	
	main_fd_pool_count= n;
	main_fd_pool_size_each= m;
}

const struct option_table_entry_s {
	char shortname;
	const char *longname;
	int argc;
	void (*handler)(char **argv);
	const char *help;
	const char *argname;
} option_table[]= {
	{  0 , "version",      0, show_version,         "display version info", "" },
	{ 'h', "help",         0, show_help,            "display quick usage synopsys", "" },
	{ 'v', "verbose",      0, set_opt_verbose,      "enable next level of logging (debug, trace)", "" },
	{ 'q', "quiet",        0, set_opt_quiet,        "hide next level of logging (info, warn, error)", "" },
	{ 'c', "config-file",  1, set_opt_configfile,   "read commands from file at startup", "PATH" },
	{  0 , "stdin",        0, set_opt_stdin,        "read commands from stdin, as a client", "" },
	{  0 , "prealloc-fd",  1, set_opt_fd_prealloc,  "Preallocate static pool of N named handles [of M bytes each]", "N[xM]" },
	{ 'M', "mlockall",     0, set_opt_mlockall,     "call mlockall after allocating memory", "" },
	{ 'F', "failsafe",     0, set_opt_failsafe,     "try not to exit, even on fatal errors", "" },
	{ 'E', "exec-on-exit", 1, set_opt_exec_on_exit, "if program exits for any reason, call exec(PROG) instead", "PROG" },
	{  0 , NULL, 0, NULL, NULL }
};

void parse_option(char shortname, char* longname, char ***argv) {
	const struct option_table_entry_s *entry;
	int i;
	
	for (entry= option_table; entry->handler; entry++) {
		if ((shortname && (shortname == entry->shortname))
			|| (longname && (0 == strcmp(longname, entry->longname)))
		) {
			for (i= 0; i < entry->argc; i++)
				if (!(*argv)[i]) {
					fatal(EXIT_BAD_OPTIONS, "Missing argument for -%c%s",
						longname? '-' : shortname, longname? longname : "");
				}
			entry->handler(*argv);
			(*argv)+= entry->argc;
			return;
		}
	}
	fatal(EXIT_BAD_OPTIONS, "Unknown option -%c%s", longname? '-' : shortname, longname? longname : "");
}

/** Parse a positive integer with size suffix.
 *
 */
int parse_size(const char *str, char **endp) {
	long i, mul= 1, factor= 1024;
	i= strtol(str, endp, 10);
	if ((*endp)[0] && (*endp)[1] == 'B')
		factor= 1000;
	switch (**endp) {
	case 'g': case 'G': mul*= factor;
	case 'm': case 'M': mul*= factor;
	case 'k': case 'K': mul*= factor;
		break;
	case 'b': case 'B': (*endp)++; break;
	case 't': case 'T': mul= LONG_MAX;
	default: break;
	}
	if (mul > 1) {
		// consume /i?B/ at the end of the suffix
		if ((*endp)[0] == 'i' && (*endp)[1] == 'B')
			(*endp) += 2;
		else if ((*endp)[0] == 'B')
			(*endp)++;
	}
	// make sure multiplied value fits in 'int'
	if (i < 0 || i * mul > INT_MAX || i * mul / mul != i) {
		errno= ERANGE;
		return i > 0? INT_MAX : 0;
	}
	return (int) i;
}

void show_help(char **argv) {
	printf("daemonproxy version %s\noptions:\n", version_git_tag);
	const struct option_table_entry_s *entry;
	for (entry= option_table; entry->handler; entry++)
		if (entry->help)
			printf("  %c%c --%-12s %5s  %s\n",
				entry->shortname? '-':' ', entry->shortname? entry->shortname : ' ',
				entry->longname, entry->argname, entry->help);
	puts("");
	
	// now exit, unless they also specified exec-on-exit
	fatal(EXIT_NO_OP, "");
}

void show_version(char **argv) {
	struct tm cal;
	localtime_r(&version_build_ts, &cal);
	printf("daemonproxy version %s\n"
		" build timestamp: %lld (%4d-%02d-%02d %02d:%02d:%02d)\n"
		" git HEAD: %s\n",
		version_git_tag, (long long) version_build_ts,
		cal.tm_year+1900, cal.tm_mon+1, cal.tm_mday, cal.tm_hour, cal.tm_min, cal.tm_sec,
		version_git_head);
	
	// now exit, unless they also specified exec-on-exit
	fatal(EXIT_NO_OP, "");
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
		log_write(LOG_LEVEL_FATAL, "%s%s", main_failsafe? "(attempting to continue) ":"", buffer);
	if (!main_failsafe)
		exit(exitcode);
}
