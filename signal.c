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

// Handle non-posix signal numbers.  Posix signals always fit in a byte.  But just in case
// some other system uses unusual numbers, we re-map the 5 that are actually specified
// to work for this program.
#define ENCODE_SIGNAL(sig) \
	((unsigned)(sig) < 128? sig \
	: (sig) == SIGINT?  128 \
	: (sig) == SIGTERM? 129 \
	: (sig) == SIGHUP?  130 \
	: (sig) == SIGUSR1? 131 \
	: (sig) == SIGUSR2? 132 \
	: 0)
#define DECODE_SIGNAL(sig) \
	((unsigned)(sig) < 128? sig \
	: (sig) == 128? SIGINT \
	: (sig) == 129? SIGTERM \
	: (sig) == 130? SIGHUP \
	: (sig) == 131? SIGUSR1 \
	: (sig) == 132? SIGUSR2 \
	: 0)

void sig_handler(int sig) {
	char c= (char) ENCODE_SIGNAL(sig);
	if (c) // don't enqueue 0s
		write(sig_wake_wr, &c, 1);
	// If it fails, nothing we can do anyway, so ignore it.
}

bool sig_dispatch();

void sig_init() {
	int pipe_fd[2];
	struct sigaction act, act_chld, act_ign;
	
	// Create pipe and set non-blocking
	if (pipe(pipe_fd)
		|| fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK)
		|| fcntl(pipe_fd[1], F_SETFL, O_NONBLOCK)
	) {
		perror("signal pipe setup");
		abort();
	}
	sig_wake_rd= pipe_fd[0];
	sig_wake_wr= pipe_fd[1];
	
	// set signal handlers
	memset(&act, 0, sizeof(act));
	act.sa_handler= (void(*)(int)) sig_handler;
	if (sigaction(SIGINT, &act, NULL)
		|| sigaction(SIGHUP,  &act, NULL)
		|| sigaction(SIGTERM, &act, NULL)
		|| sigaction(SIGUSR1, &act, NULL)
		|| sigaction(SIGUSR2, &act, NULL)
		|| (act.sa_handler= SIG_DFL, sigaction(SIGCHLD, &act, NULL))
		|| (act.sa_handler= SIG_IGN, sigaction(SIGPIPE, &act, NULL))
	) {
		perror("signal handler setup");
		abort();
	}
}

void sig_run(wake_t *wake) {
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
	int i, n;
	
	while (1) {
		// drain the signal pipe as much as possible (pipe is nonblocking)
		if (queue_n < sizeof(queue)) {
			n= read(sig_wake_rd, queue + queue_n, sizeof(queue) - queue_n);
			if (n > 0)
				queue_n += n;
			else if (!queue_n)
				return true;
		}
		// deliver as many notifications as posible
		for (i= 0; i < queue_n; i++) {
			if (!ctl_notify_signal(DECODE_SIGNAL(queue[i] & 0xFF))) {
				// controller output is blocked, so shift remaining queue to start of buffer
				// and resume here next time
				if (i > 0) {
					memmove(queue, queue + i, queue_n - i);
					queue_n -= i;
					return false;
				}
			}
		}
		queue_n= 0;
	}
}

#define CASE_SIG(sig) case sig: return #sig;
const char* sig_name(int sig_num) {
	switch (sig_num) {
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
