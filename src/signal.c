/* signal.c - routines for signal handling
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

// Global signal self-pipe
pid_t sig_main_pid= 0;
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
static int64_t sig_pipe_retry= 0;

void sig_handler(int sig) {
	int i;
	int saved_errno= errno;
	static int64_t prev= 0;
	int64_t now= gettime_mon_frac();
	
	// Make absolutely sure a forked child never runs the parent's
	// signal handler
	if (getpid() != sig_main_pid) {
		sig_reset_for_exec();
		kill(getpid(), sig);
	}

	if (sig != SIGCHLD) {
		// We have a requirement that no two signals share a timestamp.
		if (now == prev) now++;
		// We also use '0' as a NULL value, so avoid it
		if (!now) now++;
		prev= now;
		// See if we have a slot for this signal already
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
		// Record the receipt of the signal in the slot
		signals[i].number_pending++;
		signals[i].last_received_ts= now;
	}
	
	// put character in pipe to wake main loop
	if (sig_wake_wr >= 0 && write(sig_wake_wr, "", 1) != 1)
		signal_error= errno;
	errno= saved_errno;
}

void fatal_sig_handler(int sig) {
	const char* signame;
	
	// Make absolutely sure a forked child never runs the parent's
	// signal handler
	if (getpid() != sig_main_pid) {
		sig_reset_for_exec();
		kill(getpid(), sig);
	}
	
	signame= sig_name_by_num(sig);
	fatal(EXIT_BROKEN_PROGRAM_STATE, "Received signal %s%s (%d)", signame? "SIG":"???", signame? signame : "", sig);
	// No fallback available.  Probably can't actually recover from fatal signal...
	// TODO: consider some sort of longjmp + state recovery
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
	{ SIGQUIT, sig_handler },
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

bool sig_init() {
	int pipe_fd[2]= { -1, -1 }, saved_errno;
	struct sigaction act;
	struct signal_spec_s *ss;
	sigset_t previous_mask;
	const char *failed= "signal handler setup";

	// Block handlers while installing them and replacing their wake pipe.
	memset(&act, 0, sizeof(act));
	sigfillset(&act.sa_mask);
	if (sigprocmask(SIG_BLOCK, &act.sa_mask, &previous_mask) != 0) {
		log_error("blocking signals for setup: %s", strerror(errno));
		return false;
	}
	if (!sig_main_pid) {
		sig_mask= previous_mask;
		sig_main_pid= getpid();
	}

	// Install handlers even if pipe creation fails, so guarded startup can retry.
	for (ss= signal_spec; ss->signum != 0; ss++) {
		act.sa_handler= ss->handler;
		if (sigaction(ss->signum, &act, NULL) != 0)
			goto fail;
	}

	// Create pipe and set non-blocking
	failed= "signal pipe setup";
	if (fd_check_new_pair(pipe(pipe_fd), pipe_fd)
		|| fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK)
		|| fcntl(pipe_fd[1], F_SETFL, O_NONBLOCK)
	) {
		goto fail;
	}
	// Keep an existing pipe and pending events until replacement succeeds.
	if (sig_wake_rd >= 0) {
		FD_CLR(sig_wake_rd, &wake->fd_read);
		close(sig_wake_rd);
	}
	if (sig_wake_wr >= 0) close(sig_wake_wr);
	log_trace("pipe => (%d, %d)", pipe_fd[0], pipe_fd[1]);
	sig_wake_rd= pipe_fd[0];
	sig_wake_wr= pipe_fd[1];
	// Replacing the pipe must not lose a wakeup already queued on the old one.
	if (write(sig_wake_wr, "", 1) != 1)
		signal_error= errno;
	if (sigprocmask(SIG_SETMASK, &previous_mask, NULL) != 0)
		log_error("restoring signal mask: %s", strerror(errno));
	return true;

fail:
	saved_errno= errno;
	if (pipe_fd[0] >= 0) close(pipe_fd[0]);
	if (pipe_fd[1] >= 0) close(pipe_fd[1]);
	sigprocmask(SIG_SETMASK, &previous_mask, NULL);
	log_error("%s: %s", failed, strerror(saved_errno));
	errno= saved_errno;
	return false;
}

void sig_enable(bool enable) {
	sigset_t mask;
	
	if (enable)
		mask= sig_mask;
	else
		sigfillset(&mask);
	
	if (sigprocmask(SIG_SETMASK, &mask, NULL) != 0)
		perror("sigprocmask(all)");
}

/** Prepare a forked child for exec()
 *
 * This un-sets all signal handlers, and then unmasks all signals.
 * Don't do any "fatal error" checks, since we're doing things that have to succeed anyway.
 */
void sig_reset_for_exec() {
	sigset_t mask;
	struct sigaction act;
	struct signal_spec_s *ss;
	
	// unset signal handlers
	memset(&act, 0, sizeof(act));
	act.sa_handler= SIG_DFL;
	for (ss= signal_spec; ss->signum != 0; ss++)
		if (sigaction(ss->signum, &act, NULL) != 0)
			perror("sigaction(DFL)");

	// unmask all signals
	sigemptyset(&mask);
	if (sigprocmask(SIG_SETMASK, &mask, NULL) != 0)
		perror("sigprocmask(none)");
}

void sig_run() {
	char tmp[16];
	// With terminate-guard, startup can continue without a wake pipe.  Poll
	// at least once a second until resources are available to create one.
	if (sig_wake_rd < 0) {
		if ((!sig_pipe_retry || wake->now - sig_pipe_retry >= 0) && !sig_init())
			sig_pipe_retry= wake->now + (1LL << 32);
		if (sig_wake_rd < 0) {
			if (wake->next - sig_pipe_retry > 0) wake->next= sig_pipe_retry;
			return;
		}
		sig_pipe_retry= 0;
	}
	
	// Report problems from sig handler
	if (signal_error) {
		log_error("signal pipe error: %d", signal_error);
		signal_error= 0;
	}
	
	// Empty the signal pipe
	if (FD_ISSET(sig_wake_rd, &wake->fd_read))
		while (read(sig_wake_rd, tmp, sizeof(tmp)) > 0);
	
	// Wake on the signal pipe
	FD_SET(sig_wake_rd, &wake->fd_read);
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
		if (signals[i].number_pending > 0
			&& (since_ts == 0 || signals[i].last_received_ts - since_ts > 0)
			&& (oldest < 0 || signals[oldest].last_received_ts - signals[i].last_received_ts > 0)
		)
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

struct sig_list_item {
	int signum;
	union { char chars[8]; int64_t val; } signame;
};

#include "signal_data.autogen.c"

int sig_num_by_name(strseg_t name) {
	union { char chars[8]; int64_t val; } search;
	int i;
	
	if (name.len > 3 && name.data[0] == 'S' && name.data[1] == 'I' && name.data[2] == 'G') {
		name.data += 3;
		name.len -= 3;
	}
	if (name.len > 8)
		return 0;
	
	search.val= 0;
	for (i= name.len-1; i >= 0; i--)
		search.chars[i]= name.data[i];
	
	for (i= 0; sig_list[i].signum; i++) {
		if (sig_list[i].signame.val == search.val)
			return sig_list[i].signum;
	}
	return 0;
}

const char* sig_name_by_num(int signum) {
	int i;
	for (i= 0; sig_list[i].signum; i++) {
		if (sig_list[i].signum == signum)
			return sig_list[i].signame.chars;
	}
	return NULL;
}
