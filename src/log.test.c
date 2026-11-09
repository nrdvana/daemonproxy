/* gcc -Ibuild -Isrc src/log.test.c src/strseg.c -o build/log.test */
#include "config.h"
#include "daemonproxy.h"
#include "log.c"

static wake_t test_wake;
wake_t *wake= &test_wake;
struct fd_s { int num; } sink;
static bool sink_available;
fd_t *fd_by_name(strseg_t name) {
	return sink_available && !strseg_cmp(name, STRSEG("sink"))? &sink : NULL;
}
int fd_get_fdnum(fd_t *fd) { return fd->num; }

static void expect_output(int fd, const char *expected) {
	char buf[4096];
	int n= read(fd, buf, sizeof(buf));
	assert(n == strlen(expected) && !memcmp(buf, expected, n));
}

int main() {
	int fds[2], n;
	char large[2048], padding[4096];
	assert(pipe(fds) == 0);
	assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
	assert(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
	memset(large, 'x', sizeof(large)-1);
	large[sizeof(large)-1]= 0;
	memset(padding, 'p', sizeof(padding));
	log_fd= fds[1];
	log_filter= LOG_FILTER_NONE;

	// No preceding buffered data and no subsequent log call to initiate recovery.
	assert(!log_write(LOG_LEVEL_DEBUG, "%s", large));
	assert(log_msg_lost == 1 && log_buf_pos == 0);
	log_run();
	expect_output(fds[0], "warning: lost 1 log messages\n");
	assert(log_msg_lost == 0 && log_buf_pos == 0);
	assert(log_write(LOG_LEVEL_INFO, "after oversized message"));
	expect_output(fds[0], "info: after oversized message\n");

	// Preserve queued messages and the loss count while the destination is blocked.
	while ((n= write(fds[1], padding, sizeof(padding))) > 0);
	assert(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
	assert(log_write(LOG_LEVEL_INFO, "before overflow"));
	assert(log_buf_pos > 0);
	assert(!log_write(LOG_LEVEL_DEBUG, "%s", large));
	assert(!log_write(LOG_LEVEL_INFO, "also dropped"));
	assert(log_msg_lost == 2);
	log_run();
	while (read(fds[0], padding, sizeof(padding)) > 0);
	log_run();
	expect_output(fds[0], "info: before overflow\nwarning: lost 2 log messages\n");
	assert(log_write(LOG_LEVEL_INFO, "after blocked pipe"));
	expect_output(fds[0], "info: after blocked pipe\n");

	// An absent named destination must retain the warning until it appears.
	log_fd_set_name(STRSEG("sink"));
	assert(!log_write(LOG_LEVEL_DEBUG, "%s", large));
	log_run();
	assert(log_msg_lost == 1 && log_buf_pos == 0);
	sink.num= fds[1];
	sink_available= true;
	log_run();
	expect_output(fds[0], "warning: lost 1 log messages\n");
	assert(log_write(LOG_LEVEL_INFO, "after reattach"));
	expect_output(fds[0], "info: after reattach\n");
	close(fds[0]);
	close(fds[1]);
	return 0;
}
