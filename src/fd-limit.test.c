/* gcc -Ibuild -Isrc src/fd-limit.test.c -o build/fd-limit.test
 * Run: build/fd-limit.test build/daemonproxy
 */
#include "config.h"
#include <sys/resource.h>

static void check_limit(const char *daemon, const char *probe, rlim_t soft, rlim_t hard) {
	int pipefd[2], status, tries;
	pid_t pid;
	char config[4096];
	assert(pipe(pipefd) == 0);
	pid= fork();
	assert(pid >= 0);
	if (!pid) {
		struct rlimit lim= { soft, hard };
		assert(setrlimit(RLIMIT_NOFILE, &lim) == 0);
		assert(dup2(pipefd[0], 0) == 0);
		close(pipefd[0]);
		close(pipefd[1]);
		execl(daemon, daemon, "-c", "-", (char*)NULL);
		_exit(127);
	}
	close(pipefd[0]);
	int len= snprintf(config, sizeof(config),
		"service.args\tprobe\t%s\t--probe\t%llu\t%llu\n"
		"service.fds\tprobe\tnull\tcontrol.cmd\tstderr\n"
		"service.start\tprobe\n", probe,
		(unsigned long long)soft, (unsigned long long)hard);
	assert(len > 0 && len < sizeof(config));
	assert(write(pipefd[1], config, len) == len);
	close(pipefd[1]);
	for (tries= 0; tries < 100; tries++) {
		pid_t result= waitpid(pid, &status, WNOHANG);
		assert(result >= 0);
		if (result == pid) {
			assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
			return;
		}
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	assert(!"daemon limit test timed out");
}

int main(int argc, char **argv) {
	struct rlimit original;
	assert(getrlimit(RLIMIT_NOFILE, &original) == 0);
	if (argc == 4 && !strcmp(argv[1], "--probe")) {
		assert(original.rlim_cur == strtoull(argv[2], NULL, 10));
		assert(original.rlim_max == strtoull(argv[3], NULL, 10));
		if (original.rlim_cur > FD_SETSIZE) {
			int high= fcntl(2, F_DUPFD, FD_SETSIZE);
			assert(high >= FD_SETSIZE);
			close(high);
		}
		assert(write(1, "terminate\t0\n", 12) == 12);
		return 0;
	}
	assert(argc == 2 && original.rlim_max >= 64);
	rlim_t high= original.rlim_max > FD_SETSIZE? FD_SETSIZE + 1 : original.rlim_max;
	check_limit(argv[1], argv[0], high, original.rlim_max);
	check_limit(argv[1], argv[0], 64, original.rlim_max);
	check_limit(argv[1], argv[0], 64, 64);
	return 0;
}
