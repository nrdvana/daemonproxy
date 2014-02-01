#include "config.h"
#include "init-frame.h"

int control_socket= -1;
const char *control_socket_path= NULL;

void control_socket_init(const char *path) {
	struct sockaddr_un sa;
	sa.sun_family= AF_UNIX;

	// unlink anything in our way
	unlink(control_socket_path);
	
	// bind the unix socket
	if ((server_socket= socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
		log_error("socket: errno = %d", errno);
	else if (strlen(CONTROLLER_SOCKET_NAME) > sizeof(sa.sun_path))
		log_error("socket path too long for sockaddr");
	else {
		strcpy(sa.sun_path, CONTROLLER_SOCKET_NAME);
		umask(077);
		if (bind(server_socket, (struct sockaddr*) &sa, (socklen_t)sizeof(sa)) < 0) {
			close(server_socket);
			log_error("bind: errno = %d", errno);
		}
	}
}

void control_socket_destroy() {
	if (control_socket >= 0)
		close(control_socket);
	control_socket= -1;
	
	if (control_socket_path)
		unlink(control_socket_path);
	control_socket_path= NULL;
}
