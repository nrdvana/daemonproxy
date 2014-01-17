#include "config.h"
#include "init-frame.h"
#include <stdio.h>

static log_fn_t log_error_, log_warn_, log_info_, log_debug_, log_trace_;
log_fn_t *log_error= log_error_,
	*log_warn=  log_warn_,
	*log_info=  log_info_,
	*log_debug= log_null,
	*log_trace= log_null;

void log_null (const char *msg, ...) {
	// no-op
}

bool main_terminate= false;
const char *main_cfgfile= CONFIG_FILE_DEFAULT_PATH;
bool main_use_stdin= false;
bool main_mlockall= false;
wake_t main_wake;

wake_t *wake= &main_wake;

// Parse options and apply to global vars; return false if fails
bool parse_opts(char **argv);
bool parse_option(char shortname, char* longname, char ***argv);

int main(int argc, char** argv) {
	int wstat, f;
	pid_t pid;
	struct timeval tv;
	service_t *svc;
	controller_t *ctl;
	wake_t wake_instance;
	
	memset(&wake_instance, 0, sizeof(wake_instance));
	wake= &wake_instance;

	// parse arguments, overriding default values
	if (!parse_opts(argv+1)) {
		if (getpid() != 1)
			return 2;
		log_error("Invalid arguments, but continuing anyway");
	}
	
	// Set up signal handlers and signal mask and signal self-pipe
	sig_init();
	// Initialize file descriptor object pool and indexes
	fd_init(FD_POOL_SIZE, FD_OBJ_SIZE);
	// Initialize service object pool and indexes
	svc_init(SERVICE_POOL_SIZE, SERVICE_OBJ_SIZE);
	// Initialize controller state machine
	ctl_init();
	
	if (main_cfgfile) {
		f= open(main_cfgfile, O_RDONLY|O_NONBLOCK|O_NOCTTY);
		if (f == -1)
			log_error("failed to open config file \"%s\": %d", main_cfgfile, errno);
		else if (!(ctl= ctl_new(f, -1))) {
			log_error("failed to allocate controller for config file!");
			close(f);
		}
		else
			ctl_set_auto_final_newline(ctl, true);
	}
	
	if (main_use_stdin) {
		if (!(ctl= ctl_new(0, 1)))
			log_error("failed to initialize stdio controller client!");
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
		FD_ZERO(&wake->fd_read);
		FD_ZERO(&wake->fd_write);
		FD_ZERO(&wake->fd_err);
		
		// signals off
		sigset_t maskall, old;
		sigfillset(&maskall);
		if (!sigprocmask(SIG_SETMASK, &maskall, &old) == 0)
			perror("sigprocmask(all)");
	
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
			log_trace("waitpid: errno= %m");
		
		// run controller state machine
		ctl_run(wake);
		
		// run state machine of each service that is active.
		svc_run_active(wake);
		
		ctl_flush(wake);
		
		// resume normal signal mask
		if (!sigprocmask(SIG_SETMASK, &old, NULL) == 0)
			perror("sigprocmask(reset)");

		// Wait until an event or the next time a state machine needs to run
		// (state machines edit wake.next)
		// If we don't need to wait, don't.  (we don't care about select() return code)
		wake->now= gettime_mon_frac();
		if (wake->next - wake->now > 0) {
			tv.tv_sec= (long)((wake->next - wake->now) >> 32);
			tv.tv_usec= (long)((((wake->next - wake->now)&0xFFFFFFFFLL) * 1000000) >> 32);
			if (select(wake->max_fd, &wake->fd_read, &wake->fd_write, &wake->fd_err, &tv) < 0) {
				// shouldn't ever fail, but if not EINTR, at least log it and prevent
				// looping too fast
				if (errno != EINTR) {
					perror("select");
					tv.tv_usec= 500000;
					tv.tv_sec= 0;
					select(0, NULL, NULL, NULL, &tv); // use it as a sleep, this time
				}
			}
		}
	}
	
	return 0;
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

bool parse_opts(char **argv) {
	char *current;
	bool success= true;
	
	while ((current= *argv++)) {
		if (current[0] == '-' && current[1] == '-') {
			if (!parse_option(0, current+2, &argv))
				success= false;
		}
		else if (current[0] == '-') {
			for (++current; *current; current++)
				if (!parse_option(*current, NULL, &argv))
					success= false;
		}
		else {
			printf("Unexpected argument \"%s\"\n", current);
			success= false;
		}
	}
	return success;
}

bool set_opt_verbose(char**);
bool set_opt_quiet(char**);
bool set_opt_configfile(char** argv) { main_cfgfile= argv[0]; return true; }
bool set_opt_stdin(char **argv)      { main_use_stdin= true;  return true; }
bool set_opt_mlockall(char **argv)   { main_mlockall= true;   return true; }

const struct option_table_entry_s {
	char shortname;
	const char *longname;
	int argc;
	bool (*handler)(char **argv);
} option_table[]= {
	{ 'v', "verbose",      0, set_opt_verbose },
	{ 'q', "quiet",        0, set_opt_quiet },
	{ 'c', "config-file",  1, set_opt_configfile },
	{  0 , "stdin",        0, set_opt_stdin },
	{  0 , "mlockall",     0, set_opt_mlockall },
	{ 0, NULL, 0, NULL }
};

bool parse_option(char shortname, char* longname, char ***argv) {
	const struct option_table_entry_s *entry;
	int i;
	bool result;
	
	for (entry= option_table; entry->handler; entry++) {
		if ((shortname && (shortname == entry->shortname))
			|| (longname && (0 == strcmp(longname, entry->longname)))
		) {
			for (i= 0; i < entry->argc; i++)
				if (!(*argv)[i]) {
					log_error("Missing argument for -%c%s", longname? '-' : shortname, longname? longname : "");
					return false;
				}
			result= entry->handler(*argv);
			(*argv)+= entry->argc;
			return result;
		}
	}
	log_error("Unknown option -%c%s", longname? '-' : shortname, longname? longname : "");
	return false;
}

bool set_opt_verbose(char** argv) {
	if      (log_error == log_null) log_error= log_error_;
	else if (log_warn  == log_null) log_warn= log_warn_;
	else if (log_info  == log_null) log_info= log_info_;
	else if (log_debug == log_null) log_debug= log_debug_;
	else     log_trace= log_trace_;
	return true;
}

bool set_opt_quiet(char** argv) {
	if      (log_trace != log_null) log_trace= log_null;
	else if (log_debug != log_null) log_debug= log_null;
	else if (log_info  != log_null) log_info = log_null;
	else if (log_warn  != log_null) log_warn = log_null;
	else     log_error= log_null;
	return true;
}

static void log_error_(const char *msg, ...) {
	char msg2[256];
	if (snprintf(msg2, sizeof(msg2), "error: %s\n", msg) >= sizeof(msg2))
		memset(msg2+sizeof(msg2)-4, '.', 3);
	
	va_list val;
	va_start(val, msg);
	vfprintf(stderr, msg2, val);
	va_end(val);
}

static void log_warn_ (const char *msg, ...) {
	char msg2[256];
	if (snprintf(msg2, sizeof(msg2), "warning: %s\n", msg) >= sizeof(msg2))
		memset(msg2+sizeof(msg2)-4, '.', 3);
	
	va_list val;
	va_start(val, msg);
	vfprintf(stderr, msg2, val);
	va_end(val);
}

static void log_info_ (const char *msg, ...) {
	char msg2[256];
	if (snprintf(msg2, sizeof(msg2), "info: %s\n", msg) >= sizeof(msg2))
		memset(msg2+sizeof(msg2)-4, '.', 3);
	
	va_list val;
	va_start(val, msg);
	vfprintf(stderr, msg2, val);
	va_end(val);
}

static void log_debug_(const char *msg, ...) {
	char msg2[256];
	if (snprintf(msg2, sizeof(msg2), "debug: %s\n", msg) >= sizeof(msg2))
		memset(msg2+sizeof(msg2)-4, '.', 3);
	
	va_list val;
	va_start(val, msg);
	vfprintf(stderr, msg2, val);
	va_end(val);
}

static void log_trace_(const char *msg, ...) {
	char msg2[256];
	if (snprintf(msg2, sizeof(msg2), "trace: %s\n", msg) >= sizeof(msg2))
		memset(msg2+sizeof(msg2)-4, '.', 3);
	
	va_list val;
	va_start(val, msg);
	vfprintf(stderr, msg2, val);
	va_end(val);
}
