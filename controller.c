#include "config.h"
#include "init-frame.h"

#define CTL_STATE_UNDEF     0
#define CTL_STATE_CFGFILE   1
#define CTL_STATE_NONE      2
#define CTL_STATE_STARTED   3
#define CTL_STATE_SENDSTATE 4
#define CTL_STATE_RUNNING   5
#define CTL_STATE_REAPED    6

typedef struct controller_s {
	int  state;
	char exec_buf[CONFIG_SERVICE_EXEC_BUF_SIZE];
	int  arg_count, env_count;
	const char* config_file;
	int  recv_fd, send_fd;
	char in_buf[CONTROLLER_IN_BUF_SIZE];
	int  in_buf_len;
	char out_buf[CONTROLLER_OUT_BUF_SIZE];
	int  out_buf_len;
	bool out_buf_overflow;
} controller_t;

controller_t controller= {
	.state=    CTL_STATE_UNDEF,
	.exec_buf= CONTROLLER_DEFAULT_PATH "\0" CONTROLLER_DEFAULT_PATH,
	.arg_count= 2,
	.env_count= 0,
	.config_file= CONFIG_FILE_DEFAULT_PATH,
	.recv_fd= -1,
	.send_fd= -1,
	.in_buf_len= 0,
	.out_buf_len= 0,
	.out_buf_overflow= false,
};

void ctl_run(controller_t *ctl, wake_t *wake) {
	switch (ctl->state) {
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

