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

// We watch SIGCHLD, SIGINT, SIGHUP, SIGTERM, SIGUSR1, SIGUSR2
volatile int got_sigint= 0, got_sighup= 0, got_sigterm= 0, got_sigusr1= 0, got_sigusr2= 0;

void sig_handler(int sig) {
	switch (sig) {
	case SIGINT: got_sigint= 1; break;
	case SIGHUP: got_sighup= 1; break;
	case SIGTERM: got_sigterm= 1; break;
	case SIGUSR1: got_sigusr1= 1; break;
	case SIGUSR2: got_sigusr2= 1; break;
	}
	write(sig_wake_wr, "", 1);
}

void sig_setup() {
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
	char buf[32];
	
	// drain the signal pipe (pipe is nonblocking)
	while (read(sig_wake_rd, buf, sizeof(buf)) > 0);
	
	// report caught signals
	if (got_sigint) ctl_queue_message("signal INT");
	if (got_sighup) ctl_queue_message("signal HUP");
	if (got_sigterm) ctl_queue_message("signal TERM");
	if (got_sigusr1) ctl_queue_message("signal USR1");
	if (got_sigusr2) ctl_queue_message("signal USR2");
	
	// reset flags
	got_sigint= got_sighup= got_sigterm= got_sigusr1= got_sigusr2= 0;

	// Always watch signal-pipe
	FD_SET(sig_wake_rd, &wake->fd_read);
	FD_SET(sig_wake_rd, &wake->fd_err);
	if (sig_wake_rd > wake->max_fd)
		wake->max_fd= sig_wake_rd;
}

