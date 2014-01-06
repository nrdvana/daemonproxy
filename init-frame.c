#include "config.h"
#include "init-frame.h"

bool main_terminate= false;

// Parse options and apply to global vars; return false if fails
bool parse_opts(char **argv);

int main(int argc, char** argv) {
	wake_t wake;
	int wstat;
	pid_t pid;
	struct timeval tv;
	service_t *svc;
	memset(&wake, 0, sizeof(wake));

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
	wake.now= gettime_us();
	while (!main_terminate) {
		// set our wait parameters so other methods can inject new wake reasons
		wake.next= wake.now + 60*1000000; // wake at least every 60 seconds
		FD_ZERO(&wake.fd_read);
		FD_ZERO(&wake.fd_write);
		FD_ZERO(&wake.fd_err);
		
		// signals off
		sigset_t maskall, old;
		sigfillset(&maskall);
		if (!sigprocmask(SIG_SETMASK, &maskall, &old) == 0)
			perror("sigprocmask(all)");
	
		// report signals and set read-wake on signal fd
		sig_run(&wake);
		
		// reap all zombies, possibly waking services
		while ((pid= waitpid(-1, &wstat, WNOHANG)) > 0)
			if ((svc= svc_by_pid(pid)))
				svc_handle_reaped(svc, wstat);
		
		// run controller state machine
		ctl_run(&wake);
		
		// run state machine of each service that is active.
		svc_run_active(&wake);
		
		// resume normal signal mask
		if (!sigprocmask(SIG_SETMASK, &old, NULL) == 0)
			perror("sigprocmask(reset)");

		// Wait until an event or the next time a state machine needs to run
		// (state machines edit wake.next)
		// If we don't need to wait, don't.  (we don't care about select() return code)
		wake.now= gettime_us();
		if (wake.next - wake.now > 0) {
			tv.tv_sec= (wake.next - wake.now) / 1000000;
			tv.tv_usec= (wake.next - wake.now) % 1000000;
			if (select(wake.max_fd, &wake.fd_read, &wake.fd_write, &wake.fd_err, &tv) < 0) {
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

int64_t gettime_us() {
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
		t.tv_sec= time(NULL);
		t.tv_nsec= 0;
	}
	return ((int64_t) t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

bool parse_opts(char **argv) {
	return true;
}