/* control-socket.c - unix socket server for controller connections
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

#define listen_io WAKE_SOCKET_SLOT
struct sockaddr_un control_socket_addr;

void control_socket_init() {
	memset(&control_socket_addr, 0, sizeof(control_socket_addr));
	control_socket_addr.sun_family= AF_UNIX;
	listen_io->fd= -1;
}

void control_socket_run() {
	int client;
	controller_t *ctl;
	
	if (listen_io->fd >= 0) {
		if (listen_io->revents & POLLIN) {
			log_debug("control_socket is ready for accept()");
			ctl= ctl_alloc();
			if (!ctl) {
				log_warn("No free controller handlers to accpet socket connection");
				wake_at(wake->now + (5LL << 32));
				listen_io->events= 0;
				return;
			}
			client= accept(listen_io->fd, NULL, NULL);
			if (client < 0) {
				log_error("accept: %s", strerror(errno));
				ctl_free(ctl);
			}
			if (!ctl_ctor(ctl, client, client)) {
				log_error("can't create controller handler");
				ctl_dtor(ctl);
				ctl_free(ctl);
				close(client);
			}
		}
		listen_io->events |= POLLIN;
	}
}

static bool remove_any_socket(const char *path) {
	struct stat st;
	// unlink existing socket, but only if a socket owned by our UID.
	if (stat(path, &st) < 0)
		return true;
	
	if (!(st.st_mode & S_IFSOCK) || !(st.st_uid == geteuid() || st.st_uid == geteuid()))
		return false;
	
	if (unlink(path) == 0) {
		log_info("Unlinked control socket %s", path);
		return true;
	}
	else {
		log_error("Can't unlink control socket: %s", strerror(errno));
		return false;
	}
}

bool control_socket_start(strseg_t path) {
	if (path.len >= sizeof(control_socket_addr.sun_path)) {
		errno= ENAMETOOLONG;
		return false;
	}

	// If currently listening, clean that up first
	if (listen_io->fd >= 0)
		control_socket_stop();
	
	// copy new name into address
	memcpy(control_socket_addr.sun_path, path.data, path.len);
	control_socket_addr.sun_path[path.len]= '\0';
	if (!control_socket_addr.sun_path[0]) {
		errno= EINVAL;
		return false;
	}
	
	// If this is a new name, possibly remove any leftover socket in our way
	if (!remove_any_socket(control_socket_addr.sun_path))
		return false;
	
	// create the unix socket
	listen_io->fd= socket(PF_UNIX, SOCK_STREAM, 0);
	if (listen_io->fd < 0) {
		log_error("socket: %s", strerror(errno));
		return false;
	}
	
	if (bind(listen_io->fd, (struct sockaddr*) &control_socket_addr, (socklen_t) sizeof(control_socket_addr)) < 0)
		log_error("bind(control_socket, %s): %s", control_socket_addr.sun_path, strerror(errno));
	else if (listen(listen_io->fd, 2) < 0)
		log_error("listen(control_socket): %s", strerror(errno));
	else if (!fd_set_nonblock(listen_io->fd))
		log_error("fcntl(control_socket, O_NONBLOCK): %s", strerror(errno));
	else {
		listen_io->events |= POLLIN;
		return true;
	}
	
	control_socket_stop();
	return false;
}

void control_socket_stop() {
	if (listen_io->fd >= 0) {
		close(listen_io->fd);
		listen_io->fd= -1;
		remove_any_socket(control_socket_addr.sun_path);
	};
}
