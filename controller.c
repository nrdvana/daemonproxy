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
	bool (*state_fn)(struct controller_s*, wake_t*);
	
	int  script_pipe[2][2];

	int  recv_fd;
	bool recv_eof;
	char in_buf[CONTROLLER_IN_BUF_SIZE];
	int  in_buf_pos;
	bool in_buf_ignore;
	bool in_buf_overflow;
	bool in_eof;
	
	int  send_fd;
	char out_buf[CONTROLLER_OUT_BUF_SIZE];
	int  out_buf_pos;
	bool out_buf_overflow;
	int64_t out_buf_stalled;
	
	char statedump_current[NAME_MAX+1];
	int  statedump_part;
	
	//char exec_buf[CONFIG_SERVICE_EXEC_BUF_SIZE];
	//int  arg_count, env_count;
} controller_t;

controller_t controller;

// Each of the following functions returns true/false of whether to continue
//  processing (true), or yield until later (false).

static bool ctl_state_cfg_file(controller_t *ctl, wake_t *wake);
static bool ctl_state_script(controller_t *ctl, wake_t *wake);
static bool ctl_state_script_statedump(controller_t *ctl, wake_t *wake);
static bool ctl_state_script_statedump_fd(controller_t *ctl, wake_t *wake);
static bool ctl_state_script_statedump_svc(controller_t *ctl, wake_t *wake);
static bool ctl_state_script_run(controller_t *ctl, wake_t *wake);
static bool ctl_read_more(controller_t *ctl);
static bool ctl_next_command(controller_t *ctl, int *cmd_len_out);
static void ctl_process_command(controller_t *ctl, const char *str);
static bool ctl_flush_outbuf();

void ctl_init(const char* cfg_file, bool use_stdin) {
	memset(&controller, 0, sizeof(controller_t));
	controller.state_fn= &ctl_state_cfg_file;
	controller.config_path= cfg_file;
	controller.send_fd= -1;
	controller.recv_fd= -1;
	// create input / output pipes for talking to controller script
	if (pipe(controller.script_pipe[0]) || pipe(controller.script_pipe[1])) {
		perror("pipe");
		abort();
	}
	// If controller_script is "-", we use stdin/stdout
	if (use_stdin) {
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
	int n;
	// First, open config file if not open yet
	if (ctl->recv_fd == -1) {
		ctl->recv_eof= 0;
		ctl->recv_fd= open(ctl->config_path, O_RDONLY|O_NONBLOCK|O_NOCTTY);
		if (ctl->recv_fd == -1)
			ctl_notify_error("error opening config file \"%s\": %m", ctl->config_path);
	}
	// process config file lines
	while (ctl->out_buf_pos == 0 && ctl_next_command(ctl, &n)) {
		ctl->in_buf[n++]= '\0';
		ctl_process_command(ctl, ctl->in_buf);
		ctl->in_buf_pos-= n;
		memmove(ctl->in_buf, ctl->in_buf + n, ctl->in_buf_pos);
	}
	if (ctl->recv_eof) {
		// If EOF and buffer not consumed, make sure buffer ends with "\n".
		// We know there is room for "\n" because we would never call read() if the buffer
		// were full.
		if (ctl->in_buf_pos > 0 && ctl->in_buf[ctl->in_buf_pos-1] != '\n') {
			ctl->in_buf[ctl->in_buf_pos++]= '\n';
			// back to loop one last time
			return true;
		}
		// if EOF and buffer is consumed, close config file
		else if (ctl->in_buf_pos == 0) {
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
	if (ctl->out_buf_pos)
		return false; // require empty output buffer
	while ((fd= fd_iter_next(fd, ctl->statedump_current))) {
		if (!fd_notify_state(fd)) {
			// if state dump couldn't complete (output buffer full)
			//  save our position to resume later
			strcpy(ctl->statedump_current, fd_get_name(fd)); // length of name has already been checked
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
	const char *strings;
	service_t *svc= svc_by_name(ctl->statedump_current, false);
	if (!svc) ctl->statedump_part= 0;
	switch (ctl->statedump_part) {
	case 0:
		while ((svc= svc_iter_next(svc, ctl->statedump_current))) {
			strcpy(ctl->statedump_current, svc_get_name(svc)); // length of name has already been checked
	case 1:
			if (!svc_notify_state(svc)) {
				ctl->statedump_part= 1;
				return false;
			}
	case 2:
			if (!ctl_notify_svc_meta(svc_get_name(svc), svc_get_meta(svc))) {
				ctl->statedump_part= 2;
				return false;
			}
	case 3:
			if (!ctl_notify_svc_argv(svc_get_name(svc), svc_get_argv(svc))) {
				ctl->statedump_part= 3;
				return false;
			}
	case 4:
			if (!ctl_notify_svc_fds(svc_get_name(svc), svc_get_fds(svc))) {
				ctl->statedump_part= 4;
				return false;
			}
		}
	}
	ctl->state_fn= ctl_state_script_run;
	return true;
}

bool ctl_state_script_run(controller_t *ctl, wake_t *wake) {
	int n;
	// process commands while available and no output queued
	while (ctl->out_buf_pos == 0 && ctl_next_command(ctl, &n)) {
		ctl->in_buf[n++]= '\0';
		ctl_process_command(ctl, ctl->in_buf);
		ctl->in_buf_pos-= n;
		memmove(ctl->in_buf, ctl->in_buf + n, ctl->in_buf_pos);
	}
	return false;
}

void ctl_run(wake_t *wake) {
	controller_t *ctl= &controller;
	int n;
	// if anything in output buffer, try writing it
	if (ctl->out_buf_pos)
		ctl_flush_outbuf();
	// Run iterations of state machine while state returns true
	while (ctl->state_fn && ctl->state_fn(ctl, wake)) {}
	// If incoming fd, wake on data available, unless input buffer full
	// (this could also be the config file, initially)
	if (ctl->recv_fd != -1 && ctl->in_buf_pos < CONTROLLER_IN_BUF_SIZE) {
		FD_SET(ctl->recv_fd, &wake->fd_read);
		FD_SET(ctl->recv_fd, &wake->fd_err);
		if (ctl->recv_fd > wake->max_fd)
			wake->max_fd= ctl->recv_fd;
	}
	// If anything was left un-written, wake on writable pipe
	// Also, set/check timeout for writes
	if (ctl->send_fd != -1 && ctl->out_buf_pos > 0) {
		if (!ctl_flush_outbuf()) {
			// If this is the first write which has not succeeded, then we set the
			// timestamp for the write timeout.
			if (!ctl->out_buf_stalled) {
				// we use 0 as a "not stalled" flag, so if the actual timestamp is 0 fudge it to 1
				ctl->out_buf_stalled= wake->now? wake->now : 1;
			} else if (wake->now - ctl->out_buf_stalled > CONTROLLER_WRITE_TIMEOUT) {
				// TODO: kill controller script
			}
			FD_SET(ctl->send_fd, &wake->fd_write);
			FD_SET(ctl->send_fd, &wake->fd_err);
			if (ctl->send_fd > wake->max_fd)
				wake->max_fd= ctl->send_fd;
			if (wake->next - ctl->out_buf_stalled > 0)
				wake->next= ctl->out_buf_stalled;
		}
	}
}

// Read more controller input from recv_fd
bool ctl_read_more(controller_t *ctl) {
	int n;
	if (ctl->recv_fd == -1 || ctl->in_buf_pos >= CONTROLLER_IN_BUF_SIZE)
		return false;
	n= read(ctl->recv_fd, ctl->in_buf + ctl->in_buf_pos, CONTROLLER_IN_BUF_SIZE - ctl->in_buf_pos);
	if (n <= 0) {
		if (n == 0)
			ctl->in_eof= 1;
		else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			perror("read(controller pipe");
		return false;
	}
	ctl->in_buf_pos += n;
	return true;
}

// Read command from recv_fd, and process it
bool ctl_next_command(controller_t *ctl, int *cmd_len_out) {
	int prev;
	char *eol;
	while (1) {
		// see if we have a full line in the input.  else read some more.
		eol= (char*) memchr(ctl->in_buf, '\n', ctl->in_buf_pos);
		while (!eol) {
			// if buffer is full, then command is too big, and we replace everything til
			// end of next line with the string "overflow".  This strange handling of
			// converting long lines into an actual command "overflow" was helpful because
			// it reduces the number of input-reading states that we can be in, and makes
			// this function the only place where we ever need to worry about the overflow flag.
			// (other than resetting the pipe)
			if (!ctl->in_buf_ignore && ctl->in_buf_pos >= CONTROLLER_IN_BUF_SIZE) {
				ctl->in_buf_ignore= true;
				if (ctl->in_buf[0] == '#' || ctl->in_buf[0] == ' ' || ctl->in_buf[0] == '\t') {
					ctl->in_buf_pos= 1;
				} else {
					memcpy(ctl->in_buf, "overflow", 8);
					ctl->in_buf_pos= 8;
				}
			}
			prev= ctl->in_buf_pos;
			// If can't read more, then next command not available
			if (!ctl_read_more(ctl))
				return false;
			// See if new data has a "\n"
			eol= (char*) memchr(ctl->in_buf + prev, '\n', ctl->in_buf_pos - prev);
			if (ctl->in_buf_overflow) {
				// Resolve overflow condition if found end of line
				if (eol) {
					ctl->in_buf_pos -= (eol - ctl->in_buf) - prev;
					memmove(ctl->in_buf + prev, eol, ctl->in_buf_pos);
					eol= ctl->in_buf + prev;
					ctl->in_buf_ignore= false;
				}
				else ctl->in_buf_pos= prev;
			}
		}
		// Ignore empty lines, lines starting with #, and lines that start with whitespace
		if (eol > ctl->in_buf
			&& ctl->in_buf[0] != '#'
			&& ctl->in_buf[0] != ' '
			&& ctl->in_buf[0] != '\t')
			break;
		// discard otherwise
		memmove(ctl->in_buf, eol+1, ctl->in_buf_pos - (eol - ctl->in_buf) - 1);
	}
	if (cmd_len_out) *cmd_len_out= eol - ctl->in_buf;
	return true;
}

static void ctl_process_command(controller_t *ctl, const char *str) {
	if (strncmp(str, "echo", 4) == 0) {
		ctl_write("%s\n", str);
	}
//	else if ()
//	else if ()
	else {
		// unrecognized command? then reply with error, but processed it fully, so return true.
		if (strlen(str) > 30)
			ctl_notify_error("Invalid command: \"%.30s...", str);
		else
			ctl_notify_error("Invalid command: \"%s\"...", str);
	}
}

// Try to write data to the controller script, nonblocking (or possibly stdout)
// Return true if the message was queued, or false if it can't be written.
// If the caller can't queue the event that generated the message, they need to call
//  ctl_write_overflow().
bool ctl_write(const char *fmt, ... ) {
	// all writes fail on overflow condition until buffer becomes empty
	if (controller.out_buf_overflow)
		return false;
	// try writing message into buffer
	va_list val;
	va_start(val, fmt);
	int n= vsnprintf(
		controller.out_buf + controller.out_buf_pos,
		CONTROLLER_OUT_BUF_SIZE - controller.out_buf_pos,
		fmt, val);
	va_end(val);
	// If message doesn't fit, flush buffer, and if that doesn't make it fit, return false.
	if (n >= CONTROLLER_OUT_BUF_SIZE - controller.out_buf_pos) {
		// See if we can flush output buffer to free up space
		ctl_flush_outbuf();
		if (n >= CONTROLLER_OUT_BUF_SIZE - controller.out_buf_pos)
			return false;
		// If we have the space now, repeat the sprintf, assuming that n is idential to before.
		va_start(val, fmt);
		vsnprintf(
			controller.out_buf + controller.out_buf_pos,
			CONTROLLER_OUT_BUF_SIZE - controller.out_buf_pos,
			fmt, val);
		va_end(val);
	}
	return true;
}

// Mark the output buffer as missing a critical state event.  When the controller script starts
// reading the pipe again, we will send it an "overflow" event to tell it that it needs to
// re-sync its state.
void ctl_notify_overflow() {
	controller.out_buf_overflow= true;
}

// Try to flush the output buffer (nonblocking)
// Return true if flushed completely.  false otherwise.
static bool ctl_flush_outbuf() {
	controller_t *ctl= &controller;
	int n;
	while (ctl->out_buf_pos) {
		// if no send_fd, discard buffer
		if (ctl->send_fd == -1)
			ctl->out_buf_pos= 0;
		// else write as much as we can (on nonblocking fd)
		else {
			n= write(ctl->send_fd, ctl->out_buf, ctl->out_buf_pos);
			if (n > 0) {
				ctl->out_buf_pos -= n;
				ctl->out_buf_stalled= 0;
				memmove(ctl->out_buf, ctl->out_buf + n, ctl->out_buf_pos);
			}
			else {
				// we have an error, or the write would block.
				return false;
			}
		}
	}
	// If we just finished emptying the buffer, and the overflow flag is set,
	// then send the overflow message.
	if (controller.out_buf_overflow) {
		memcpy(ctl->out_buf, "overflow\n", 9);
		ctl->out_buf_pos= 9;
		controller.out_buf_overflow= false;
		return ctl_flush_outbuf();
	}
	return true;
}

bool ctl_notify_signal(int sig_num) {
	return ctl_write("signal %d %s\n", sig_num, sig_name(sig_num));
}

bool ctl_notify_svc_state(const char *name, int64_t up_ts, int64_t reap_ts, pid_t pid, int wstat) {
	if (!up_ts)
		return ctl_write("service.state	%s	down\n", name);
	else if ((up_ts - wake->now) >= 0)
		return ctl_write("service.state	%s	starting	%d\n", name, up_ts>>32);
	else if (!reap_ts)
		return ctl_write("service.state	%s	up	%d	pid	%d\n", name, up_ts>>32, (int) pid);
	else if (WIFEXITED(wstat))
		return ctl_write("service.state	%s	down	%d	exit	%d	uptime %d	pid	%d\n",
			name, (int)(reap_ts>>32), WEXITSTATUS(wstat), (int)((reap_ts-up_ts)>>32), (int) pid);
	else
		return ctl_write("service.state	%s	down	%d	signal	%d=%s	uptime %d	pid	%d\n",
			name, (int)(reap_ts>>32), WTERMSIG(wstat), sig_name(WTERMSIG(wstat)),
			(int)((reap_ts-up_ts)>>32), (int) pid);
}

bool ctl_notify_svc_meta(const char *name, const char *tsv_fields) {
	return ctl_write("service.meta	%s	%s\n", name, tsv_fields);
}

bool ctl_notify_svc_argv(const char *name, const char *tsv_fields) {
	return ctl_write("service.args	%s	%s\n", name, tsv_fields);
}

bool ctl_notify_svc_fds(const char *name, const char *tsv_fields) {
	return ctl_write("service.fds	%s	%s\n", name, tsv_fields);
}

bool ctl_notify_fd_state(const char *name, const char *file_path, const char *pipe_read, const char *pipe_write) {
	if (file_path)
		return ctl_write("fd.state	%s	file	%s\n", name, file_path);
	else if (pipe_read)
		return ctl_write("fd.state	%s	pipe_from	%s\n", name, pipe_read);
	else if (pipe_write)
		return ctl_write("fd.state	%s	pipe_to	%s\n", name, pipe_write);
	else
		return ctl_write("fd.state	%s	unknown\n", name);
}
