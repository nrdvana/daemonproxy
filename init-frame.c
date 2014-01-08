#include "config.h"
#include "init-frame.h"

static log_fn_t log_error_, log_warn_, log_info_, log_debug_, log_trace_;
log_fn_t *log_error= log_error_,
	*log_warn=  log_warn_,
	*log_info=  log_info_,
	*log_debug= log_debug_,
	*log_trace= log_trace_;

void log_null (const char *msg, ...) {
	// no-op
}

bool main_terminate= false;
wake_t wake_instance;
wake_t *wake= &wake_instance;

// Parse options and apply to global vars; return false if fails
bool parse_opts(char **argv);

int main(int argc, char** argv) {
	int wstat;
	pid_t pid;
	struct timeval tv;
	service_t *svc;
	memset(&wake_instance, 0, sizeof(wake_instance));

	// parse arguments, overriding default values
	if (!parse_opts(argv))
		return 2;
	
	// Set up signal handlers and signal mask and signal self-pipe
	sig_init();
	// Initialize file descriptor object pool and indexes
	fd_init(FD_POOL_SIZE, FD_OBJ_SIZE);
	// Initialize service object pool and indexes
	svc_init(SERVICE_POOL_SIZE, SERVICE_OBJ_SIZE);
	// Initialize controller state machine and pipes
	ctl_init(CONFIG_FILE_DEFAULT_PATH, true);
	
	// Lock all memory into ram. init should never be "swapped out".
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
		perror("mlockall");
	
	// terminate is disabled when running as init, so this is an infinite loop
	// (except when debugging)
	wake->now= gettime_mon_frac();
	while (!main_terminate) {
		// set our wait parameters so other methods can inject new wake reasons
		wake->next= wake->now + (60LL<<32); // wake at least every 60 seconds
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
		while ((pid= waitpid(-1, &wstat, WNOHANG)) > 0)
			if ((svc= svc_by_pid(pid)))
				svc_handle_reaped(svc, wstat);
		
		// run controller state machine
		ctl_run(wake);
		
		// run state machine of each service that is active.
		svc_run_active(wake);
		
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
	return true;
}

#include <stdio.h>
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
