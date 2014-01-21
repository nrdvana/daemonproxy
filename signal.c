#include "config.h"
#include "init-frame.h"

//---------------------------
// Note on signal handling:
//
// I'm aware that there are more elegant robust ways to write signal handling,
// but frankly, I'm sick of all the song and dance and configure tests, and I
// decided to just go the absolute minimal most portable route this time.

// Global signal self-pipe
int sig_wake_rd= -1;
int sig_wake_wr= -1;
volatile int signal_error= 0;

// Handle non-posix signal numbers.  Posix signals always fit in a byte.  But just in case
// some other system uses unusual numbers, we re-map the 5 that this program needs to be able
// to relay *iff* they are wider than 7 bits
#define ENCODE_SIGNAL(sig) \
	((unsigned)(sig) < 128? sig \
	: (sig) == SIGINT?  128 \
	: (sig) == SIGTERM? 129 \
	: (sig) == SIGHUP?  130 \
	: (sig) == SIGUSR1? 131 \
	: (sig) == SIGUSR2? 132 \
	: (sig) == SIGCHLD? 133 \
	: 0)
#define DECODE_SIGNAL(sig) \
	((unsigned)(sig) < 128? sig \
	: (sig) == 128? SIGINT \
	: (sig) == 129? SIGTERM \
	: (sig) == 130? SIGHUP \
	: (sig) == 131? SIGUSR1 \
	: (sig) == 132? SIGUSR2 \
	: (sig) == 133? SIGCHLD \
	: 0)

void sig_handler(int sig) {
	char c= (char) ENCODE_SIGNAL(sig);
	if (c) // don't enqueue 0s
		if (write(sig_wake_wr, &c, 1) != 1)
			// If it fails, flag the error
			signal_error= errno;
}

void fatal_sig_handler(int sig) {
	fatal(EXIT_BROKEN_PROGRAM_STATE, "Received signal %s (%d)", sig_name(sig), sig);
	// probably can't actually recover from fatal signal...
	exit(EXIT_BROKEN_PROGRAM_STATE);
}

bool sig_dispatch();

struct signal_spec_s {
	int signum;
	void (*handler)(int);
} signal_spec[]= {
	{ SIGINT,   sig_handler },
	{ SIGHUP,   sig_handler },
	{ SIGTERM,  sig_handler },
	{ SIGUSR1,  sig_handler },
	{ SIGUSR2,  sig_handler },
	{ SIGCHLD,  sig_handler },
	{ SIGPIPE,  SIG_IGN },
	{ SIGABRT,  fatal_sig_handler },
	{ SIGFPE,   fatal_sig_handler },
	{ SIGILL,   fatal_sig_handler },
	{ SIGSEGV,  fatal_sig_handler },
	{ SIGBUS,   fatal_sig_handler },
	{ SIGTRAP,  fatal_sig_handler },
	{ 0, NULL }
};

void sig_init() {
	int pipe_fd[2];
	struct sigaction act;
	struct signal_spec_s *ss;
	
	// Create pipe and set non-blocking
	if (pipe(pipe_fd)
		|| fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK)
		|| fcntl(pipe_fd[1], F_SETFL, O_NONBLOCK)
	) {
		fatal(EXIT_IMPOSSIBLE_SCENARIO, "signal pipe setup: errno = %d", errno);
	}
	sig_wake_rd= pipe_fd[0];
	sig_wake_wr= pipe_fd[1];
	
	// set signal handlers
	memset(&act, 0, sizeof(act));
	for (ss= signal_spec; ss->signum != 0; ss++) {
		act.sa_handler= ss->handler;
		if (sigaction(ss->signum, &act, NULL))
			fatal(EXIT_IMPOSSIBLE_SCENARIO, "signal handler setup: errno = %d", errno);
	}
}

void sig_run(wake_t *wake) {
	if (signal_error) {
		log_error("signal pipe error: %d", signal_error);
		signal_error= 0;
	}
	// If we delivered all of the notifications, wake on the signal pipe
	if (sig_dispatch()) {
		FD_SET(sig_wake_rd, &wake->fd_read);
		FD_SET(sig_wake_rd, &wake->fd_err);
		if (sig_wake_rd > wake->max_fd)
			wake->max_fd= sig_wake_rd;
	}
	// else we're waiting on the controller write pipe.
}

bool sig_dispatch() {
	static char queue[32];
	static int queue_n= 0;
	int i, n, sig;
	
	while (1) {
		// drain the signal pipe as much as possible (pipe is nonblocking)
		if (queue_n < sizeof(queue)) {
			n= read(sig_wake_rd, queue + queue_n, sizeof(queue) - queue_n);
			if (n > 0) {
				log_trace("%d signals dequeued", n);
				queue_n += n;
			}
			else {
				log_trace("signal pipe read errno: %d", errno);
				if (!queue_n)
					return true;
			}
		}
		log_debug("%d signals to deliver", n);
		// deliver as many notifications as posible
		for (i= 0; i < queue_n; i++) {
			log_debug("deliver signal %d (%s)", queue[i], sig_name(queue[i]));
			sig= DECODE_SIGNAL(queue[i] & 0xFF);
			if (sig != SIGCHLD) {
				if (!ctl_notify_signal(NULL, DECODE_SIGNAL(queue[i] & 0xFF))) {
					// controller output is blocked, so shift remaining queue to start of buffer
					// and resume here next time
					if (i > 0) {
						memmove(queue, queue + i, queue_n - i);
						queue_n -= i;
						return false;
					}
				}
			}
		}
		queue_n= 0;
	}
}

#define CASE_SIG(sig) case sig: return #sig;
const char* sig_name(int sig_num) {
	switch (sig_num) {
	CASE_SIG(SIGCHLD)
	CASE_SIG(SIGHUP)
	CASE_SIG(SIGINT)
	CASE_SIG(SIGQUIT)
	CASE_SIG(SIGILL)
	CASE_SIG(SIGTRAP)
	CASE_SIG(SIGABRT)
	CASE_SIG(SIGBUS)
	CASE_SIG(SIGFPE)
	CASE_SIG(SIGKILL)
	CASE_SIG(SIGUSR1)
	CASE_SIG(SIGUSR2)
	CASE_SIG(SIGSEGV)
	CASE_SIG(SIGPIPE)
	CASE_SIG(SIGALRM)
	CASE_SIG(SIGTERM)
	default: return "unknown";
	}
}
