#include "config.h"
#include "init-frame.h"

#define CTL_STATE_UNDEF             0
#define CTL_STATE_CFGFILE           1
#define CTL_STATE_SCRIPT_START      2
#define CTL_STATE_SCRIPT_SEND_FD    3
#define CTL_STATE_SCRIPT_SEND_SVC   4
#define CTL_STATE_SCRIPT_RUN        5
#define CTL_STATE_SCRIPT_RESTART    6
#define CTL_STATE_NONE              7

typedef struct controller_s {
	int  state;
	int64_t script_start_time;
	pid_t script_pid;
	char script_reset_iter[NAME_MAX+1];
	const char *config_path, *script_path;
	int  recv_fd, send_fd;
	char in_buf[CONTROLLER_IN_BUF_SIZE];
	int  in_buf_len;
	char out_buf[CONTROLLER_OUT_BUF_SIZE];
	int  out_buf_len;
	bool out_buf_overflow;
	//char exec_buf[CONFIG_SERVICE_EXEC_BUF_SIZE];
	//int  arg_count, env_count;
} controller_t;

controller_t controller;

void ctl_init(const char* cfg_file, const char *controller_script) {
	memset(&controller, 0, sizeof(controller_t));
	controller.state= CTL_STATE_CFGFILE;
	controller.config_path= cfg_file;
	controller.script_path= controller_script;
	controller.send_fd= -1;
	controller.recv_fd= -1;
}

void ctl_run(wake_t *wake) {
	controller_t *ctl= &controller;
	int n;
	// if anything in output buffer, try writing it
	if (ctl->out_buf_len)
		ctl_flush_outbuf();
	
	switch (ctl->state) {
	case CTL_STATE_CFGFILE:
		// First, open config file if not open yet
		if (ctl->recv_fd == -1) {
			ctl->recv_fd= open(ctl->config_file, O_RDONLY);
			if (ctl->recv_fd == -1)
				ctl_notify_error("error opening config file \"%s\": %m", ctl->config_file);
		}
		// process config file lines
		while (ctl_next_command()) {}
		
		if (ctl->recv_fd == -1)
			// If we get here, we reached the end of the config file (and recv_fd == -1)
			ctl->state= CTL_STATE_SCRIPT;
		else
			break;

	case CTL_STATE_SCRIPT_START: ctl_state_script_start:
		if (!ctl_spawn_script()) {
			ctl->state= CTL_STATE_NONE;
			goto ctl_state_none;
		}
		ctl->script_reset_iter[0]= '\0';
		ctl->state= CTL_STATE_SCRIPT_SEND_FD;
	case CTL_STATE_SCRIPT_SEND_FD:
		fd= NULL;
		while (1) {
			fd= fd_iter_next(fd, ctl->script_reset_iter);
			if (!fd) {
				ctl->state= CTL_STATE_SCRIPT_SEND_SVC;
				break;
			}
			if (!fd_dump_state(fd)) {
				// if state dump couldn't complete (output buffer full)
				//  save our position to resume later
				strlcpy(ctl->script_reset_iter, fd_name(fd), sizeof(ctl->script_reset_iter));
				break;
			}
		}
		if (ctl->state == CTL_STATE_SCRIPT_SEND_FD)
			break;
	case CTL_STATE_SCRIPT_SEND_SVC:
		svc= NULL;
		while (1) {
			svc= svc_iter_next(svc, ctl->script_reset_iter);
			if (!svc) {
				ctl->state= CTL_STATE_SCRIPT_RUN;
				break;
			}
			if (!svc_dump_state(svc)) {
				// if state dump couldn't complete (output buffer full)
				//  save our position to resume later
				strlcpy(ctl->script_reset_iter, svc_name(svc), sizeof(ctl->script_reset_iter));
				break;
			}
		}
		if (ctl->state == CTL_STATE_SCRIPT_SEND_SVC)
			break;
	case CTL_STATE_SCRIPT_RUN:
		
		while (ctl_next_command()) {}
		
		if (ctl->recv_fd == -1)
			// If we get here, the controller script died!
			// Restart it after a delay.
			ctl->state= CTL_STATE_SCRIPT_RESTART;
			ctl->script_restart_time= wake->now + CTL_SCRIPT_RESTART_DELAY;
		}
		else
			break;
	case CTL_STATE_SCRIPT_RESTART:
		if (wake->now - ctl->script_restart_time > 0) {
			ctl->state= CTL_STATE_SCRIPT_START;
			goto ctl_state_script_start;
		}
		else if (wake->next - ctl->script_restart_time > 0)
			wake->next= ctl->script_restart_time;
		break;
		
	case CTL_STATE_NONE: ctl_state_none:
		// If no controller, do nothing until SIGHUP
		break;
	default:
		ctl_notify_error("Invalid controller state! %d", ctl->state);
		ctl->state= CTL_STATE_SCRIPT_RUN; // safe recovery default?
	}
	
	// If incoming fd, wake on data available
	// (this could also be the config file, initially)
	if (ctl->recv_fd != -1) {
		FD_SET(ctl->recv_fd, &wake->fd_read);
		FD_SET(ctl->recv_fd, &wake->fd_err);
		if (ctl->recv_fd > wake->max_fd)
			wake->max_fd= ctl->revc_fd;
	}
	// if anything was left un-written, wake on writable pipe
	if (ctl->send_fd != -1 && ctl->out_buf_len > 0) {
		FD_SET(ctl->send_fd, &wake->fd_write);
		FD_SET(ctl->send_fd, &wake->fd_err);
		if (ctl->send_fd > wake->max_fd)
			wake->max_fd= ctl->send_fd;
	}
}

void ctl_flush_outbuf() {
	controller_t *ctl= &controller;
	int n;
	while (ctl->out_buf_len) {
		// if no send_fd, discard buffer
		if (ctl->send_fd == -1)
			ctl->out_buf_len= 0;
		// else write as much as we can (on nonblocking fd)
		else {
			n= write(ctl->send_fd, ctl->out_buf, ctl->out_buf_len);
			if (n > 0) {
				ctl->out_buf_len -= n;
				memmove(ctl->out_buf, ctl->out_buf + n, ctl->out_buf_len);
			}
			else if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				break;
			else
				ctl_kill_script();
		}
	}
}

ctl_kill_script() {
	controller.recv_fd= -1;
	controller.send_fd= -1;
	if (controller.script_pid) {
		kill(controller.script_pid, SIGTERM);
		controller.script_pid= 0;
	}
}

