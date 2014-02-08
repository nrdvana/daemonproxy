#include "config.h"
#include "daemonproxy.h"

// Global signal self-pipe
int sig_wake_rd= -1;
int sig_wake_wr= -1;
volatile int signal_error= 0;

typedef struct sig_status_s {
	int signum;
	int64_t last_received_ts;
	int number_pending;
} sig_status_t;

#define SIGNAL_STATUS_SLOTS 16
volatile sig_status_t signals[SIGNAL_STATUS_SLOTS];
volatile int signals_count= 0;
static sigset_t sig_mask;

void sig_handler(int sig) {
	int i;
	static int64_t prev= 0;
	int64_t now= gettime_mon_frac();

	// We have a requirement that no two signals share a timestamp.
	if (now == prev) now++;
	// We also use '0' as a NULL value, so avoid it
	if (!now) now++;
	prev= now;
	
	for (i= 0; i < signals_count; i++)
		if (signals[i].signum == sig)
			break;
	
	// Not seen before? allocate next signal slot
	if (i == signals_count) {
		if (signals_count < SIGNAL_STATUS_SLOTS)
			signals_count++;
		else // none free? fail by re-using final slot
			signals[--i].number_pending= 0;
		signals[i].signum= sig;
	}
	
	signals[i].number_pending++;
	signals[i].last_received_ts= now;
	
	// put character in pipe to wake main loop
	if (write(sig_wake_wr, "", 1) != 1)
		signal_error= errno;
}

void fatal_sig_handler(int sig) {
	const char* signame= sig_name_by_num(sig);
	fatal(EXIT_BROKEN_PROGRAM_STATE, "Received signal %s%s (%d)", signame? "SIG":"???", signame? signame : "", sig);
	// No fallback available.  Probably can't actually recover from fatal signal...
	// TODO: consider some sort of longjmp + state recovery
	exit(EXIT_BROKEN_PROGRAM_STATE);
}

struct signal_spec_s {
	int signum;
	void (*handler)(int);
} signal_spec[]= {
	{ SIGINT,  sig_handler },
	{ SIGHUP,  sig_handler },
	{ SIGTERM, sig_handler },
	{ SIGUSR1, sig_handler },
	{ SIGUSR2, sig_handler },
	{ SIGCHLD, sig_handler },
	{ SIGPIPE, SIG_IGN },
	{ SIGABRT, fatal_sig_handler },
	{ SIGFPE,  fatal_sig_handler },
	{ SIGILL,  fatal_sig_handler },
	{ SIGSEGV, fatal_sig_handler },
	{ SIGBUS,  fatal_sig_handler },
	{ SIGTRAP, fatal_sig_handler },
	{ 0, NULL }
};

void sig_init() {
	int pipe_fd[2];
	struct sigaction act;
	struct signal_spec_s *ss;
	
	// initialize with 0
	memset((void*)signals, 0, sizeof(signals));
	
	// Create pipe and set non-blocking
	if (pipe(pipe_fd)
		|| fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK)
		|| fcntl(pipe_fd[1], F_SETFL, O_NONBLOCK)
	) {
		fatal(EXIT_IMPOSSIBLE_SCENARIO, "signal pipe setup: %s", strerror(errno));
	}
	log_trace("pipe => (%d, %d)", pipe_fd[0], pipe_fd[1]);
	sig_wake_rd= pipe_fd[0];
	sig_wake_wr= pipe_fd[1];
	
	// set signal handlers
	memset(&act, 0, sizeof(act));
	for (ss= signal_spec; ss->signum != 0; ss++) {
		act.sa_handler= ss->handler;
		if (sigaction(ss->signum, &act, NULL))
			fatal(EXIT_IMPOSSIBLE_SCENARIO, "signal handler setup: %s", strerror(errno));
	}
	
	// capture our signal mask
	if (!sigprocmask(SIG_SETMASK, NULL, &sig_mask) == 0)
		perror("sigprocmask(all)");
}

void sig_enable(bool enable) {
	sigset_t mask;
	
	if (enable)
		mask= sig_mask;
	else
		sigfillset(&mask);
	
	if (!sigprocmask(SIG_SETMASK, &mask, NULL) == 0)
		perror("sigprocmask(all)");
}

void sig_run() {
	char tmp[16];
	
	// Report problems from sig handler
	if (signal_error) {
		log_error("signal pipe error: %d", signal_error);
		signal_error= 0;
	}
	
	// Empty the signal pipe
	while (read(sig_wake_rd, tmp, sizeof(tmp)) > 0);
	
	// Wake on the signal pipe
	FD_SET(sig_wake_rd, &wake->fd_read);
	FD_SET(sig_wake_rd, &wake->fd_err);
	if (sig_wake_rd > wake->max_fd)
		wake->max_fd= sig_wake_rd;
}

/** Get the next event (in chronological order) from the specified timestamp.
 * If the timestamp is the special value 0, it will return the oldest event.
 * 
 * This returns the sugnal number, what it was last seen, and how many there have been.
 * The controller may then call sig_mark_seen(signum, count) to decrement the count back to 0.
 * Does not return any records with a count of 0.
 *
 * NOTE: must be called while signals are blocked!
 */
bool sig_get_new_events(int64_t since_ts, int *sig_out, int64_t *ts_out, int *count_out) {
	int i, oldest;
	// first, find oldest with nonzero count
	for (i= 0, oldest= -1; i < signals_count; i++) {
		if (signals[i].number_pending > 0 && signals[i].last_received_ts - since_ts > 0
			&& (oldest < 0 || signals[oldest].last_received_ts - signals[i].last_received_ts > 0))
			oldest= i;
	}
	if (oldest < 0)
		return false;
	
	if (sig_out)   *sig_out=   signals[oldest].signum;
	if (ts_out)    *ts_out=    signals[oldest].last_received_ts;
	if (count_out) *count_out= signals[oldest].number_pending;
	return true;
}

/** Subtract a number from a signal's entry, to state that it has been dealt with
 */
void sig_mark_seen(int signum, int count) {
	int i;
	for (i= 0; i < signals_count; i++) {
		if (signals[i].signum == signum) {
			signals[i].number_pending-= count;
			if (signals[i].number_pending < 0)
				signals[i].number_pending= 0;
			return;
		}
	}
}

#include "signal_data.autogen.c"
