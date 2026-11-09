/* gcc -Ibuild -Isrc src/signal.test.c src/strseg.c -o build/signal.test */
#include "config.h"
#include "daemonproxy.h"

static int pipe_error, fcntl_failure, fcntl_calls, pipe_calls, high_pipe;
static int created[2]= { -1, -1 };
static int fake_pipe(int *fds) {
	pipe_calls++;
	created[0]= created[1]= -1;
	if (pipe_error) { errno= pipe_error; return -1; }
	int result= pipe(fds);
	if (!result && high_pipe) {
		int high= fcntl(fds[0], F_DUPFD, FD_SETSIZE);
		assert(high >= FD_SETSIZE);
		close(fds[0]);
		fds[0]= high;
	}
	if (!result) { created[0]= fds[0]; created[1]= fds[1]; }
	return result;
}
static int fake_fcntl(int fd, int cmd, ...) {
	va_list args;
	va_start(args, cmd);
	int val= va_arg(args, int);
	va_end(args);
	if (++fcntl_calls == fcntl_failure) { errno= EIO; return -1; }
	return fcntl(fd, cmd, val);
}
#define pipe fake_pipe
#define fcntl fake_fcntl
#include "signal.c"
#undef pipe
#undef fcntl

static wake_t test_wake;
wake_t *wake= &test_wake;
int64_t gettime_mon_frac() { return wake->now; }
bool log_write(int level, const char *msg, ...) { return true; }
void fatal(int code, const char *msg, ...) { assert(!"sig_init must report failure to its caller"); }

int main() {
	int sig, count, old_rd, old_wr, i;
	int64_t ts;
	sigset_t mask;
	struct sigaction action;
	wake->now= 10LL << 32;
	wake->next= 200LL << 32;
	wake->max_fd= -1;
	pipe_error= EMFILE;
	assert(!sig_init() && errno == EMFILE);
	assert(sig_wake_rd == -1 && sig_wake_wr == -1);
	assert(sigaction(SIGUSR1, NULL, &action) == 0 && action.sa_handler == sig_handler);
	assert(sigismember(&action.sa_mask, SIGUSR2) == 1);
	errno= EBUSY;
	raise(SIGUSR1);
	assert(errno == EBUSY && signal_error == 0);
	assert(sig_get_new_events(0, &sig, &ts, &count) && sig == SIGUSR1 && count == 1);
	sig_run();
	assert(wake->max_fd == -1 && wake->next == wake->now + (1LL << 32));
	i= pipe_calls;
	sig_run();
	assert(pipe_calls == i); // Busy controller activity must not cause a retry loop.
	pipe_error= 0;
	wake->now= wake->next;
	sig_run();
	assert(sig_wake_rd >= 0 && sig_wake_wr >= 0);
	assert(FD_ISSET(sig_wake_rd, &wake->fd_read));
	assert(sig_get_new_events(0, &sig, &ts, &count) && count == 1);
	old_rd= sig_wake_rd;
	old_wr= sig_wake_wr;

	// Each fcntl failure must close both temporary ends and retain the old pipe.
	for (i= 1; i <= 4; i++) {
		fcntl_calls= 0;
		fcntl_failure= i;
		assert(!sig_init() && errno == EIO);
		assert(sig_wake_rd == old_rd && sig_wake_wr == old_wr);
		assert(fcntl(old_rd, F_GETFD) >= 0 && fcntl(old_wr, F_GETFD) >= 0);
		assert(fcntl(created[0], F_GETFD) == -1 && errno == EBADF);
		assert(fcntl(created[1], F_GETFD) == -1 && errno == EBADF);
	}
	fcntl_failure= 0;
	int probe= fcntl(old_rd, F_DUPFD, FD_SETSIZE);
	if (probe >= 0) {
		close(probe);
		high_pipe= 1;
		assert(!sig_init() && errno == EMFILE);
		assert(sig_wake_rd == old_rd && sig_wake_wr == old_wr);
		assert(fcntl(created[0], F_GETFD) == -1 && errno == EBADF);
		assert(fcntl(created[1], F_GETFD) == -1 && errno == EBADF);
		high_pipe= 0;
	}
	sig_enable(false);
	assert(sig_init());
	assert(fcntl(old_rd, F_GETFD) == -1 && errno == EBADF);
	assert(fcntl(old_wr, F_GETFD) == -1 && errno == EBADF);
	assert(!FD_ISSET(old_rd, &wake->fd_read));
	assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && sigismember(&mask, SIGUSR1));
	sig_enable(true);
	assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 && !sigismember(&mask, SIGUSR1));
	raise(SIGUSR1);
	assert(sig_get_new_events(0, &sig, &ts, &count) && count == 2);
	assert(fcntl(sig_wake_rd, F_GETFD) & FD_CLOEXEC);
	assert(fcntl(sig_wake_wr, F_GETFD) & FD_CLOEXEC);
	assert(fcntl(sig_wake_rd, F_GETFL) & O_NONBLOCK);
	assert(fcntl(sig_wake_wr, F_GETFL) & O_NONBLOCK);
	close(sig_wake_rd);
	close(sig_wake_wr);
	sig_wake_rd= sig_wake_wr= -1;
	sig_reset_for_exec();
	return 0;
}
