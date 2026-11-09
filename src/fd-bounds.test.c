/* gcc -Ibuild -Isrc src/fd-bounds.test.c -o build/fd-bounds.test */
#include "config.h"
#include "daemonproxy.h"
#include <sys/resource.h>

static void expect_closed(int fd) {
	assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

int main() {
	int base, high, boundary, pair[2], first, second, which;
	struct rlimit limit;
	assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
	if (limit.rlim_cur <= FD_SETSIZE + 1) {
		puts("Skipping actual high-descriptor tests: inherited limit too low");
		return 0;
	}
	base= open("/dev/null", O_RDONLY);
	assert(base >= 0 && base < FD_SETSIZE);
	assert(fd_check_new(base) == base);
	boundary= fcntl(base, F_DUPFD, FD_SETSIZE - 1);
	assert(boundary == FD_SETSIZE - 1 && fd_check_new(boundary) == boundary);
	close(boundary);
	high= fcntl(base, F_DUPFD, FD_SETSIZE);
	assert(high >= FD_SETSIZE);
	assert(fd_check_new(high) == -1 && errno == EMFILE);
	expect_closed(high);
	errno= ENFILE;
	assert(fd_check_new(-1) == -1 && errno == ENFILE);

	for (which= 0; which < 3; which++) {
		assert(pipe(pair) == 0);
		if (which != 1) {
			high= fcntl(pair[0], F_DUPFD, FD_SETSIZE);
			assert(high >= FD_SETSIZE);
			close(pair[0]);
			pair[0]= high;
		}
		if (which != 0) {
			high= fcntl(pair[1], F_DUPFD, FD_SETSIZE);
			assert(high >= FD_SETSIZE);
			close(pair[1]);
			pair[1]= high;
		}
		first= pair[0]; second= pair[1];
		assert(fd_check_new_pair(0, pair) == -1 && errno == EMFILE);
		assert(pair[0] == -1 && pair[1] == -1);
		expect_closed(first);
		expect_closed(second);
	}
	assert(pipe(pair) == 0);
	assert(fd_check_new_pair(0, pair) == 0);
	assert(fcntl(pair[0], F_GETFD) >= 0 && fcntl(pair[1], F_GETFD) >= 0);
	close(pair[0]); close(pair[1]);
	pair[0]= pair[1]= base;
	errno= ENFILE;
	assert(fd_check_new_pair(-1, pair) == -1 && errno == ENFILE);
	assert(fcntl(base, F_GETFD) >= 0);
	close(base);
	return 0;
}
