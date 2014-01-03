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
	const char *config_path, *script_path;
	bool (state_fn*)(struct controller_s*, wake_t*);
	
	int  script_pipe[2][2];

	int  recv_fd;
	bool recv_eof;
	char in_buf[CONTROLLER_IN_BUF_SIZE];
	int  in_buf_len;
	bool in_buf_overflow;
	int  cmd_len;
	
	int  send_fd;
	char out_buf[CONTROLLER_OUT_BUF_SIZE];
	int  out_buf_len;
	bool out_buf_overflow;
	
	char statedump_current[NAME_MAX+1];
	int  statedump_part;
	
	//char exec_buf[CONFIG_SERVICE_EXEC_BUF_SIZE];
	//int  arg_count, env_count;
} controller_t;

controller_t controller;

// Each of the following functions returns true/false of whether to continue
//  processing (true), or yield until later (false).
bool ctl_next_command();
typedef bool (controller_state_fn)(controller_t *ctl, wake_t *wake);
controller_state_fn
	ctl_state_cfg_file,
	ctl_state_script;

void ctl_flush_outbuf();
void ctl_kill_script();

void ctl_init(const char* cfg_file, const char *controller_script) {
	memset(&controller, 0, sizeof(controller_t));
	controller.state_fn= &ctl_state_cfg_file;
	controller.config_path= cfg_file;
	controller.script_path= controller_script;
	controller.send_fd= -1;
	controller.recv_fd= -1;
	controller.cmd_len= -1;
	// create input / output pipes for talking to controller script
	if (pipe(controller.script_pipe[0]) || pipe(controller.script_pipe[1])) {
		perror("pipe");
		abort();
	}
	// If controller_script is "-", we use stdin/stdout
	if (controller_script && controller_script[0] == '-' && controller_script[1] == '\0') {
		// We deliberately leak pipe FDs here.  We will never read/write the pipes,
		// but they still need to be open or any (accidentally configured) controller
		// script would restart endlessly on SIGPIPE.
		controller.script_pipe[0][0]= 0;
		controller.script_pipe[1][1]= 1;
	}
	// read end of input pipe and write end of output pipe must be nonblocking
	if (fcntl(controller.script_pipe[0][0], F_SETFL, O_NONBLOCK)
		|| fcntl(controller.script_pipe[1][1], F_SETFL, O_NONBLOCK)
	) {
		perror("fcntl(O_NONBLOCK)");
		abort();
	}
}

bool ctl_state_cfg_file(controller_t *ctl, wake_t *wake) {
	// First, open config file if not open yet
	if (ctl->recv_fd == -1) {
		ctl->recv_eof= 0;
		ctl->recv_fd= open(ctl->config_file, O_RDONLY|O_NONBLOCK|O_NOCTTY);
		if (ctl->recv_fd == -1)
			ctl_notify_error("error opening config file \"%s\": %m", ctl->config_file);
	}
	// process config file lines
	while (ctl->out_buf_len == 0 && ctl_next_command(ctl) && ctl_process_command(ctl)) {
		n= ctl->cmd_len + 1;
		ctl->in_buf_len-= n;
		memmove(ctl->in_buf, ctl->in_buf + n, ctl->in_buf_len);
	}
	if (ctl->recv_eof) {
		// If EOF and buffer not consumed, make sure buffer ends with "\n".
		// We know there is room for "\n" because we would never call read() if the buffer
		// were full.
		if (ctl->in_buf_len > 0 && ctl->in_buf[ctl->in_buf_len-1] != '\n') {
			ctl->in_buf[ctl->in_buf_len++]= '\n';
			// back to loop one last time
			return true;
		}
		// if EOF and buffer is consumed, close config file
		else if (ctl->in_buf_len == 0) {
			close(ctl->recv_fd);
			ctl->recv_fd= -1;
		}
	}
	// If end of config (or failed to open) pass control to "script" state
	if (ctl->recv_fd == -1) {
		ctl->state_fn= &ctl_state_script;
		return true;
	}
	return false;
}

bool ctl_state_script(controller_t *ctl, wake_t *wake) {
	ctl->recv_fd= ctl->script_pipe[0][0];
	ctl->send_fd= ctl->script_pipe[1][1];
	ctl->state_fn= ctl_state_script_statedump;
	return true;
}

bool ctl_state_script_statedump(controller_t *ctl, wake_t *wake) {
	ctl->statedump_current[0]= '\0';
	ctl->statedump_part= 0;
	// Dump FDs first
	ctl->state_fn= ctl_state_script_statedump_fd;
	return true;
}

bool ctl_state_script_statedump_fd(controller_t *ctl, wake_t *wake) {
	fd_t *fd= NULL;
	if (ctl->out_buf_len)
		return false; // require empty output buffer
	while ((fd= fd_iter_next(fd, ctl->statedump_current))) {
		ctl_notify_fd_state(fd);
		
			// if state dump couldn't complete (output buffer full)
			//  save our position to resume later
			strlcpy(ctl->statedump_current, fd_name(fd), sizeof(ctl->statedump_current));
			return false;
		}
	}
	ctl->state_fn= ctl_state_script_statedump_svc;
	ctl->statedump_current[0]= '\0';
	ctl->statedump_part= 0;
	return true;
}

bool ctl_state_script_statedump_svc(controller_t *ctl, wake_t *wake) {
	int n;
	char *strings;
	svc_t *svc= svc_by_name(ctl->statedump_current);
	if (!svc) ctl->statedump_part= 0;
	switch (ctl->statedump_part) {
	case 0:
		while ((svc= svc_iter_next(svc, ctl->statedump_current))) {
			strlcpy(ctl->statedump_current, svc_name(svc), sizeof(ctl->statedump_current));
	case 1:
			if (ctl->out_buf_len) {
				ctl->statedump_part= 1;
				return false;
			}
			svc_notify_status(svc);
	case 2:
			if (ctl->out_buf_len) {
				ctl->statedump_part= 2;
				return false;
			}
			svc_get_meta(svc, &n, &strings);
			ctl_notify_svc_meta(svc_name(svc), n, strings);
	case 3:
			if (ctl->out_buf_len) {
				ctl->statedump_part= 3;
				return false;
			}
			svc_get_args(svc, &n, &strings);
			ctl_notify_svc_args(svc_name(svc), n, strings);
	case 4:
			if (ctl->out_buf_len) {
				ctl->statedump_part= 4;
				return false;
			}
			svc_get_fds(svc, &n, &strings);
			ctl_notify_svc_args(svc_name(svc), n, strings);
		}
	}
	ctl->state_fn= ctl_state_script_run;
	return true;
}

bool ctl_state_script_run(controller_t *ctl, wake_t *wake) {
	// process commands while available and no output queued
	while (ctl->out_buf_len == 0 && ctl_next_command(ctl) && ctl_process_command(ctl)) {
		n= ctl->cmd_len + 1;
		ctl->in_buf_len-= n;
		memmove(ctl->in_buf, ctl->in_buf + n, ctl->in_buf_len);
	}
	return false;
}

void ctl_run(wake_t *wake) {
	controller_t *ctl= &controller;
	int n;
	// if anything in output buffer, try writing it
	if (ctl->out_buf_len)
		ctl_flush_outbuf();
	// always load available data into the input buffer, to help prevent controller
	//  script from blocking on writing the pipe
	ctl_read_all_available

	
	// If incoming fd, wake on data available, unless input buffer full
	// (this could also be the config file, initially)
	if (ctl->recv_fd != -1 && ctl->in_buf_len < CONTROLLER_IN_BUF_SIZE) {
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

// Read more controller input from recv_fd
bool ctl_read_more(controller *ctl) {
	int n;
	if (ctl->recv_fd == -1 || ctl->in_buf_len >= CONTROLLER_IN_BUF_SIZE)
		return false;
	n= read(ctl->recv_fd, ctl->in_buf + ctl->in_buf_len, CONTROLLER_IN_BUF_SIZE - ctl->in_buf_len);
	if (n <= 0) {
		if (n == 0)
			ctl->in_eof= 1;
		else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			perror("read(controller pipe");
		return false;
	}
	if (ctl->in_buf_overflow) { 
	ctl->in_buf_len += n;
	return true;
}

// Read command from read_fd, and process it
bool ctl_next_command(controller_t *ctl) {
	int prev;
	while (1) {
		// see if we have a full line in the input.  else read some more.
		char *eol= (char*) memchr(ctl->in_buf, '\n', ctl->in_buf_len);
		while (!eol) {
			// if buffer is full, then command is too big, and we replace everything til
			// end of next line with the string "overflow".  This strange handling of
			// converting long lines into an actual command "overflow" was helpful because
			// it reduces the number of input-reading states that we can be in, and makes
			// this function the only place where we ever need to worry about the overflow flag.
			// (other than resetting the pipe)
			if (!ctl->in_buf_ignore && ctl->in_buf_len >= CONTROLLER_IN_BUF_SIZE) {
				ctl->in_buf_ignore= true;
				if (ctl->in_buf[0] == '#' || ctl->in_buf[0] == ' ' || ctl->in_buf[0] == '\t') {
					ctl->in_buf_len= 1;
				} else {
					memcpy(ctl->in_buf, "overflow", 8);
					ctl->in_buf_len= 8;
				}
			}
			prev= ctl->in_buf_len;
			// If can't read more, then next command not available
			if (!ctl_read_more(ctl))
				return false;
			// See if new data has a "\n"
			eol= (char*) memchr(ctl->in_buf + prev, '\n', ctl->in_buf_len - prev);
			if (ctl->in_buf_overflow) {
				// Resolve overflow condition if found end of line
				if (eol) {
					ctl->in_buf_len -= (eol - ctl->in_buf) - prev;
					memmove(ctl->in_buf + prev, eol, ctl->in_buf_len);
					eol= ctl->in_buf + prev;
					ctl->in_buf_ignore= false;
				}
				else ctl->in_buf_len= prev;
			}
		}
		// Ignore empty lines, lines starting with #, and lines that start with whitespace
		if (eol > ctl->in_buf
			&& ctl->in_buf[0] != '#'
			&& ctl->in_buf[0] != ' '
			&& ctl->in_buf[0] != '\t')
			break;
		// discard otherwise
		memmove(ctl->in_buf, eol+1, ctl->in_buf_len - (eol - ctl->in_buf) - 1);
	}
	ctl->cmd_len= eol - ctl->in_buf;
	return true;
}

void ctl_process_command(char *buf) {
	if ()
	else if ()
	else if ()
	else {
		// unrecognized command? then reply with error, but processed it fully, so return true.
		if (strlen(cmd) > 30)
			ctl_notify_error("Invalid command: \"%.30s...", cmd);
		else
			ctl_notify_error("Invalid command: \"%s\"...", cmd);
		return true;
	}
}

bool ctl_notify_signal(int sig_num) {
	
}

bool ctl_notify_svc_start(char *name);
bool ctl_notify_svc_up(char *name, double uptime, pid_t pid);
bool ctl_notify_svc_down(char *name, double downtime, double uptime, int wstat, pid_t pid);
bool ctl_notify_svc_meta(char *name, int meta_count, char *meta_series);
bool ctl_notify_svc_args(char *name, int arg_count, char *arg_series);
bool ctl_notify_svc_fds(char *name, int fd_count, char *fd_series);
bool ctl_notify_error(char *msg, ...);

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

void ctl_kill_script() {
	controller.recv_fd= -1;
	controller.send_fd= -1;
	if (controller.script_pid) {
		kill(controller.script_pid, SIGTERM);
		controller.script_pid= 0;
	}
}

