/* Fault injection for the accept/constructor failure paths.
 * gcc -Ibuild -Isrc src/control-socket.test.c -o build/control-socket.test
 */
#include "config.h"
#include "daemonproxy.h"

struct controller_s { int allocated; } test_ctl;
static wake_t test_wake;
wake_t *wake= &test_wake;
static int accept_error, ctor_calls, free_calls, close_calls, ctor_ok;
static int accept_result= 11, high_socket;

static int test_accept(int fd, struct sockaddr *addr, socklen_t *len) {
	if (accept_error) { errno= accept_error; return -1; }
	return accept_result;
}
static int test_socket(int domain, int type, int protocol) {
	int fd= socket(domain, type, protocol);
	if (fd >= 0 && high_socket) {
		int high= fcntl(fd, F_DUPFD, FD_SETSIZE);
		assert(high >= FD_SETSIZE);
		close(fd);
		return high;
	}
	return fd;
}
static int test_close(int fd) {
	assert(fd == 11);
	close_calls++;
	return 0;
}
#define accept test_accept
#define close test_close
#define socket test_socket
#include "control-socket.c"
#undef accept
#undef close
#undef socket

bool log_write(int level, const char *msg, ...) { return true; }
controller_t *ctl_alloc() {
	assert(!test_ctl.allocated);
	test_ctl.allocated= 1;
	return &test_ctl;
}
void ctl_free(controller_t *ctl) {
	assert(ctl == &test_ctl && ctl->allocated);
	ctl->allocated= 0;
	free_calls++;
}
bool ctl_ctor(controller_t *ctl, int rd, int wr) {
	assert(ctl->allocated && rd == 11 && wr == 11);
	ctor_calls++;
	return ctor_ok;
}
void ctl_dtor(controller_t *ctl) {
	/* A failed constructor has not taken ownership of any descriptors. */
	assert(0);
}

int main() {
	int i, errors[]= { EAGAIN, EINTR, EMFILE, ENFILE };
	control_socket= 10;
	for (i= 0; i < sizeof(errors)/sizeof(*errors); i++) {
		FD_SET(control_socket, &wake->fd_read);
		accept_error= errors[i];
		control_socket_run();
		assert(!test_ctl.allocated && free_calls == i+1);
		assert(!ctor_calls && !close_calls);
		assert(FD_ISSET(control_socket, &wake->fd_read));
	}
	accept_error= 0;
	accept_result= FD_SETSIZE;
	control_socket_run();
	assert(!test_ctl.allocated && !ctor_calls && !close_calls);
	accept_result= 11;
	control_socket_run();
	assert(!test_ctl.allocated && ctor_calls == 1 && close_calls == 1);
	ctor_ok= 1;
	control_socket_run();
	assert(test_ctl.allocated && ctor_calls == 2 && close_calls == 1);
	ctl_free(&test_ctl);
	control_socket= -1;
	// A high listening socket is rejected before any FD_SET or bind.
	int probe= open("/dev/null", O_RDONLY);
	int high= fcntl(probe, F_DUPFD, FD_SETSIZE);
	close(probe);
	if (high >= 0) {
		close(high);
		high_socket= 1;
		assert(!control_socket_start(STRSEG("build/control-socket-test.sock")));
		assert(control_socket == -1 && errno == EMFILE);
		assert(fcntl(high, F_GETFD) == -1 && errno == EBADF);
	}
	return 0;
}
