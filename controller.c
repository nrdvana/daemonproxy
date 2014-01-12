#include "config.h"
#include "init-frame.h"

struct controller_s;
typedef bool ctl_state_fn_t(struct controller_s *);

typedef struct controller_s {
	const char *config_path, *script_path;
	ctl_state_fn_t *state_fn;
	
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
	int64_t out_buf_stall_time;
	
	char statedump_current[NAME_MAX+1];
	int  statedump_part;
	
	int command_len;
	int command_argc;
	char * command_argv[8];
	
	//char exec_buf[CONFIG_SERVICE_EXEC_BUF_SIZE];
	//int  arg_count, env_count;
} controller_t;

controller_t controller;

// Each of the following functions returns true/false of whether to continue
//  processing (true), or yield until later (false).

#define STATE(name, ...) static bool name(controller_t *ctl)
STATE(ctl_state_cfg_open);
STATE(ctl_state_cfg_close);
STATE(ctl_state_read_command);
STATE(ctl_state_cmd_overflow);
STATE(ctl_state_cmd_unknown);
STATE(ctl_state_cmd_statedump, "statedump");
STATE(ctl_state_cmd_statedump_fd);
STATE(ctl_state_cmd_statedump_svc);
STATE(ctl_state_cmd_svcargs, "service.args");
STATE(ctl_state_cmd_svcmeta, "service.meta");
STATE(ctl_state_cmd_svcfds,  "service.fds");

static bool ctl_read_more(controller_t *ctl);
static bool ctl_flush_outbuf();

typedef struct ctl_command_table_entry_s {
	const char *command;
	ctl_state_fn_t *state_fn;
} ctl_command_table_entry_t;

#include "controller_data.autogen.c"

const ctl_command_table_entry_t * ctl_find_command(const char *buffer) {
	const ctl_command_table_entry_t *result= &ctl_command_table[ ctl_command_hash_func(buffer) ];
	return result->state_fn && strstr(buffer, result->command) == buffer? result : NULL;
}

void ctl_init(const char* cfg_file, bool use_stdin) {
	memset(&controller, 0, sizeof(controller_t));
	controller.state_fn= &ctl_state_cfg_open;
	controller.config_path= cfg_file;
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
	// read-end of input pipe and write-end of output pipe must be nonblocking
	if (fcntl(controller.script_pipe[0][0], F_SETFL, O_NONBLOCK)
		|| fcntl(controller.script_pipe[1][1], F_SETFL, O_NONBLOCK)
	) {
		perror("fcntl(O_NONBLOCK)");
		abort();
	}
	
	controller.recv_fd= controller.script_pipe[0][0];
	controller.send_fd= controller.script_pipe[1][1];
}

#define END_CMD(cond) end_cmd_(ctl, cond)
static inline bool end_cmd_(controller_t *ctl, bool final_cond) {
	if (final_cond) {
		ctl->state_fn= ctl_state_read_command;
		return true;
	}
	return false;
}

bool ctl_state_cfg_open(controller_t *ctl) {
	int n;
	// First, open config file
	ctl->recv_eof= 0;
	ctl->recv_fd= open(ctl->config_path, O_RDONLY|O_NONBLOCK|O_NOCTTY);
	if (ctl->recv_fd == -1) {
		n= errno;
		log_error("open config file \"%s\": %d", ctl->config_path, n);
		ctl_notify_error("open config file \"%s\": %d", ctl->config_path, n);
		ctl->state_fn= ctl_state_cfg_close;
		return true;
	}
	else {
		ctl->state_fn= ctl_state_read_command;
		return true;
	}
}

bool ctl_state_cfg_close(controller_t *ctl) {
	close(ctl->recv_fd);
	ctl->recv_fd= ctl->script_pipe[0][0];
	ctl->state_fn= ctl_state_read_command;
	return true;
}

// This state is really just a fancy non-blocking readline(), with special
// handling for long lines (and EOF, when reading the config file)
bool ctl_state_read_command(controller_t *ctl) {
	int prev, n;
	char *eol, *p;
	const ctl_command_table_entry_t *cmd;

	// clean up old command, if any.
	if (ctl->command_len > 0) {
		ctl->in_buf_pos -= ctl->command_len;
		memmove(ctl->in_buf, ctl->in_buf + ctl->command_len, ctl->in_buf_pos);
		ctl->command_len= 0;
	}
	
	// see if we have a full line in the input.  else read some more.
	eol= (char*) memchr(ctl->in_buf, '\n', ctl->in_buf_pos);
	while (!eol) {
		// if buffer is full, then command is too big, and we ignore the rest of the line
		if (ctl->in_buf_pos >= CONTROLLER_IN_BUF_SIZE) {
			// In case its a comment, preserve comment character (long comments are not an error)
			ctl->in_buf_overflow= true;
			ctl->in_buf_pos= 1;
		}
		// Try reading more from our non-blocking handle
		prev= ctl->in_buf_pos;
		if (ctl_read_more(ctl)) {
			log_trace("controller read %d bytes", ctl->in_buf_pos - prev);
			// See if new data has a "\n"
			eol= (char*) memchr(ctl->in_buf + prev, '\n', ctl->in_buf_pos - prev);
		}
		else {
			log_trace("controller unable to read more yet");
			// else, if reading file, check for EOF (and append newline if needed)
			if (ctl->recv_fd != ctl->script_pipe[0][0] && ctl->in_eof) {
				// If last line missing newline, append newline and process command
				if (ctl->in_buf_pos > 0) {
					ctl->in_buf[ctl->in_buf_pos]= '\n';
					eol= ctl->in_buf + ctl->in_buf_pos++;
					break;
				}
				// else switch back to pipe and try again
				ctl->state_fn= ctl_state_cfg_close;
				return true;
			}
			// else done for now, until more data available on pipe
			return false;
		}
	}
	// We now have a complete line
	ctl->command_len= eol - ctl->in_buf + 1;
	*eol= '\0';
	log_debug("controller got line: \"%s\"", ctl->in_buf);
	// Ignore overflow lines, empty lines, lines starting with #, and lines that start with whitespace
	if (ctl->in_buf[0] == '\n'
		|| ctl->in_buf[0] == '\r'
		|| ctl->in_buf[0] == '#'
		|| ctl->in_buf[0] == ' '
		|| ctl->in_buf[0] == '\t'
	)
	{
		log_trace("Ignoring comment line");
		// continue back to this state by doing nothing
	}
	// check for command overflow
	else if (ctl->in_buf_overflow)
		ctl->state_fn= ctl_state_cmd_overflow;
	// else try to parse and dispatch it
	else {
		// as a convenience to the command, parse out its first 8 arguments (tab delimited)
		p= ctl->in_buf;
		ctl->command_argv[0]= p;
		for (ctl->command_argc= 1; ctl->command_argc < sizeof(ctl->command_argv)/sizeof(*ctl->command_argv); ctl->command_argc++) {
			p= strchr(p, '\t');
			if (!p) break;
			ctl->command_argv[ctl->command_argc]= ++p;
		}
		// Now see if we can find the command
		if ((cmd= ctl_find_command(ctl->in_buf))) {
			ctl->state_fn= cmd->state_fn;
		}
		else
			ctl->state_fn= ctl_state_cmd_unknown;
	}
	return true;
}

bool ctl_state_cmd_overflow(controller_t *ctl) {
	if (!ctl_write("overflow\n"))
		return false;
	ctl->state_fn= ctl_state_read_command;
	return true;
}

bool ctl_state_cmd_unknown(controller_t *ctl) {
	// find comand string
	const char *p= strchr(ctl->in_buf, '\t');
	int n= p? p - ctl->in_buf : strlen(ctl->in_buf);
	if (!ctl_notify_error("Unknown command: %.*s", n, ctl->in_buf))
		return false;
	ctl->state_fn= ctl_state_read_command;
	return true;
}

/** Statedump command, part 1: Initialize vars and pass to part 2.
 */
bool ctl_state_cmd_statedump(controller_t *ctl) {
	ctl->statedump_current[0]= '\0';
	ctl->statedump_part= 0;
	// Dump FDs first
	ctl->state_fn= ctl_state_cmd_statedump_fd;
	return true;
}

/** Statedump command, part 2: iterate fd objects and dump each one.
 * Uses an iterator of fd_t's name so that if fds are creeated or destroyed during our
 * iteration, we can safely resume.  (and this iteration could be idle for a while if
 * the controller script doesn't read its pipe)
 */
bool ctl_state_cmd_statedump_fd(controller_t *ctl) {
	fd_t *fd= NULL;
	while ((fd= fd_iter_next(fd, ctl->statedump_current))) {
		if (!fd_notify_state(fd)) {
			// if state dump couldn't complete (output buffer full)
			//  save our position to resume later
			strcpy(ctl->statedump_current, fd_get_name(fd)); // length of name has already been checked
			return false;
		}
	}
	ctl->state_fn= ctl_state_cmd_statedump_svc;
	ctl->statedump_current[0]= '\0';
	ctl->statedump_part= 0;
	return true;
}

/** Statedump command, part 3: iterate services and dump each one.
 * Like part 2 above, except a service has 4 lines of output, and we need to be able to resume
 * if a full buffer interrupts us half way through one service.
 */
bool ctl_state_cmd_statedump_svc(controller_t *ctl) {
	int n;
	const char *strings;
	service_t *svc= svc_by_name(ctl->statedump_current, false);
	if (!svc) ctl->statedump_part= 0;
	switch (ctl->statedump_part) {
	case 0:
		while ((svc= svc_iter_next(svc, ctl->statedump_current))) {
			log_debug("service iter = %s", svc_get_name(svc));
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
	return END_CMD(true);
}

/** service.args command
 * Set the argv for a service object
 * Also report the state change when done.
 */
bool ctl_state_cmd_svcargs(controller_t *ctl) {
	if (ctl->command_argc < 2)
		return END_CMD( ctl_notify_error("Missing service name") );
	
	if (ctl->command_argc < 3)
		return END_CMD( ctl_notify_error("Missing argument list") );
	
	ctl->command_argv[2][-1]= '\0'; // terminate service name
	if (!svc_check_name(ctl->command_argv[1]))
		return END_CMD( ctl_notify_error("Invalid service name: \"%s\"", ctl->command_argv[1]) );
	
	service_t *svc= svc_by_name(ctl->command_argv[1], true);
	if (svc_set_argv(svc, ctl->command_argv[2]))
		return END_CMD( ctl_notify_svc_argv(svc_get_name(svc), svc_get_argv(svc)) );
	else
		return END_CMD( ctl_notify_error("unable to set argv for service \"%s\"", ctl->command_argv[1]) );
	// If reporting the result above fails, the whole function will be re-run, but we don't care
	// since that isn't a common case and isn't too expensive.
}

bool ctl_state_cmd_svcmeta(controller_t *ctl) {
	ctl->state_fn= ctl_state_read_command;
	return true;
}

bool ctl_state_cmd_svcfds(controller_t *ctl) {
	ctl->state_fn= ctl_state_read_command;
	return true;
}

/** Run all processing needed for the controller for this time slice
 * This function is mainly a wrapper that repeatedly executes the current state until
 * the state_fn returns false.  We then flush buffers and decide what to wake on.
 */
void ctl_run(wake_t *wake) {
	controller_t *ctl= &controller;
	int n;
	// if anything in output buffer, try writing it
	if (ctl->out_buf_pos)
		ctl_flush_outbuf();
	// Run iterations of state machine while state returns true
	log_debug("ctl state = %s", ctl_get_state_name(ctl->state_fn));
	while (ctl->state_fn(ctl)) {
		log_debug("ctl state = %s", ctl_get_state_name(ctl->state_fn));
	}
	// If incoming fd, wake on data available, unless input buffer full
	// (this could also be the config file, initially)
	if (ctl->recv_fd != -1 && ctl->in_buf_pos < CONTROLLER_IN_BUF_SIZE) {
		log_trace("wake on controller recv_fd");
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
			if (!ctl->out_buf_stall_time) {
				// we use 0 as a "not stalled" flag, so if the actual timestamp is 0 fudge it to 1
				ctl->out_buf_stall_time= wake->now? wake->now : 1;
			} else if (wake->now - ctl->out_buf_stall_time > CONTROLLER_WRITE_TIMEOUT) {
				// TODO: kill controller script
			}
			FD_SET(ctl->send_fd, &wake->fd_write);
			FD_SET(ctl->send_fd, &wake->fd_err);
			if (ctl->send_fd > wake->max_fd)
				wake->max_fd= ctl->send_fd;
			if (wake->next - ctl->out_buf_stall_time > 0)
				wake->next= ctl->out_buf_stall_time;
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
			log_error("read(controller pipe): %d", errno);
		return false;
	}
	ctl->in_buf_pos += n;
	return true;
}

// Try to write data to the controller script, nonblocking (or possibly stdout)
// Return true if the message was queued, or false if it can't be written.
// If the caller can't queue the event that generated the message, they need to call
//  ctl_write_overflow().
bool ctl_write(const char *fmt, ... ) {
	// all writes fail on overflow condition until buffer becomes empty
	if (controller.out_buf_overflow) {
		log_debug("write ignored, overflow");
		return false;
	}
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
		log_debug("message len %d > buffer free %d", n, CONTROLLER_OUT_BUF_SIZE - controller.out_buf_pos);
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
	controller.out_buf_pos += n;
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
		log_trace("controller write buffer %d bytes pending %s",
			ctl->out_buf_pos, controller.out_buf_overflow? "(overflow flag set)" : "");
		// if no send_fd, discard buffer
		if (ctl->send_fd == -1)
			ctl->out_buf_pos= 0;
		// else write as much as we can (on nonblocking fd)
		else {
			n= write(ctl->send_fd, ctl->out_buf, ctl->out_buf_pos);
			if (n > 0) {
				log_trace("controller flushed %d bytes", n);
				ctl->out_buf_pos -= n;
				ctl->out_buf_stall_time= 0;
				memmove(ctl->out_buf, ctl->out_buf + n, ctl->out_buf_pos);
			}
			else {
				log_debug("controller outbuf write failed: $d", errno);
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
