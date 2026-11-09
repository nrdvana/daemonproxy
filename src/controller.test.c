/* Controller descriptor lifecycle regression tests.
 * Build from the repository root:
 * gcc -DCURRENT_UNIX_TIMESTAMP=0 -Ibuild -Isrc src/controller.test.c \
 *   src/fd.c src/service.c src/signal.c \
 *   src/Contained_RBTree.c src/log.c src/strseg.c src/options.c \
 *   src/control-socket.c src/version_data.autogen.c -o build/controller.test
 */
#define main daemonproxy_main
#include "daemonproxy.c"
#undef main
#include "controller.c"

int main() {
	int pair[2], saved, replacement;
	char buf[32];
	controller_t *ctl;
	signal(SIGPIPE, SIG_IGN);
	ctl_init();

	// Destroying an unconstructed controller must not close descriptor zero.
	saved= open("/dev/null", O_RDONLY);
	assert(saved >= 0);
	ctl= ctl_alloc();
	assert(ctl->recv_fd == -1 && ctl->send_fd == -1);
	assert(!fd_new_file(STRSEG("high"), FD_SETSIZE, (fd_flags_t){0}, STRSEG("test")));
	assert(errno == EMFILE);
	assert(!fd_new_pipe(STRSEG("r"), 10, STRSEG("w"), FD_SETSIZE));
	assert(errno == EMFILE);
	assert(!ctl_ctor(ctl, -2, -1));
	assert(!ctl_ctor(ctl, FD_SETSIZE, -1) && errno == EMFILE);
	assert(!ctl_ctor(ctl, -1, FD_SETSIZE) && errno == EMFILE);
	assert(ctl->recv_fd == -1 && ctl->send_fd == -1);
	ctl_dtor(ctl);
	assert(fcntl(saved, F_GETFD) >= 0);
	ctl_free(ctl);
	close(saved);

	// EOF releases input ownership while retaining output long enough to flush.
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	ctl= ctl_new(pair[0], pair[0]);
	assert(ctl);
	assert(shutdown(pair[1], SHUT_WR) == 0);
	assert(!ctl_read_more(ctl));
	assert(ctl->recv_fd == -1 && ctl->send_fd == pair[0]);
	assert(ctl_write(ctl, "reply\n") && ctl_flush_outbuf(ctl));
	assert(read(pair[1], buf, sizeof(buf)) == 6 && !memcmp(buf, "reply\n", 6));
	FD_SET(pair[0], &wake->fd_read);
	FD_SET(pair[0], &wake->fd_write);
	FD_SET(pair[0], &wake->fd_err);
	ctl_dtor(ctl);
	assert(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);
	assert(!FD_ISSET(pair[0], &wake->fd_read));
	assert(!FD_ISSET(pair[0], &wake->fd_write));
	assert(!FD_ISSET(pair[0], &wake->fd_err));
	// A subsequent destructor must not close a reused descriptor.
	replacement= open("/dev/null", O_RDONLY);
	assert(replacement == pair[0]);
	ctl_dtor(ctl);
	assert(fcntl(replacement, F_GETFD) >= 0);
	ctl_free(ctl);
	close(replacement);
	close(pair[1]);

	// Write failure discards output, then EOF releases the remaining owner.
	assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	ctl= ctl_new(pair[0], pair[0]);
	assert(ctl);
	close(pair[1]);
	assert(ctl_write(ctl, "undeliverable\n") && ctl_flush_outbuf(ctl));
	assert(ctl->send_fd == -1 && ctl->recv_fd == pair[0]);
	assert(!ctl->send_buf_pos && !ctl->send_overflow);
	assert(!ctl_read_more(ctl) && ctl->recv_fd == -1);
	assert(fcntl(pair[0], F_GETFD) < 0 && errno == EBADF);
	ctl_dtor(ctl);
	ctl_free(ctl);

	// A permanent read error must also end input processing.
	assert(socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
	ctl= ctl_new(pair[0], -1);
	assert(ctl);
	close(pair[0]);
	assert(!ctl_read_more(ctl) && ctl->recv_fd == -1);
	ctl_dtor(ctl);
	ctl_free(ctl);
	close(pair[1]);
	return 0;
}
