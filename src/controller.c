#include "config.h"
#include "daemonproxy.h"

struct controller_s;
typedef bool ctl_state_fn_t(struct controller_s *);

struct controller_s {
	ctl_state_fn_t *state_fn;
	
	int  recv_fd;
	bool append_final_newline;
	char recv_buf[CONTROLLER_RECV_BUF_SIZE];
	int  recv_buf_pos;
	bool recv_overflow;
	
	int  send_fd;
	char send_buf[CONTROLLER_SEND_BUF_SIZE];
	int  send_buf_pos;
	bool send_overflow;
	int64_t write_timeout_reset;
	int64_t write_timeout_close;
	int64_t send_blocked_ts;
	int64_t last_signal_ts;
	
	int      line_len;         // length of current command in recv_buf
	strseg_t command_name;     // str segment of command name (within recv_buf)
	strseg_t command;          // remainder of command (within recv_buf)
	const char *command_error; // error message set by commands
	char     command_error_buf[64];// buffer in case we want to write a custom error msg
	
	int     command_substate;
	char    statedump_current[NAME_LIMIT];
	int64_t statedump_ts;
};

controller_t client[CONTROLLER_MAX_CLIENTS];

// Each of the following functions returns true/false of whether to continue
//  processing (true), or yield until later (false).

#define STATE(name, ...) static bool name(controller_t *ctl)
STATE(ctl_state_next_command);
STATE(ctl_state_run_command);
STATE(ctl_state_end_command);
STATE(ctl_state_close);
STATE(ctl_state_free);
STATE(ctl_state_dump_fds);
STATE(ctl_state_dump_services);
STATE(ctl_state_dump_signals);

#define COMMAND(name, ...) static bool name(controller_t *ctl)
COMMAND(ctl_cmd_echo, "echo");
COMMAND(ctl_cmd_statedump,           "statedump");
COMMAND(ctl_cmd_svc_args,            "service.args");
COMMAND(ctl_cmd_svc_fds,             "service.fds");
COMMAND(ctl_cmd_svc_start,           "service.start");
COMMAND(ctl_cmd_svc_signal,          "service.signal");
COMMAND(ctl_cmd_fd_pipe,             "fd.pipe");
COMMAND(ctl_cmd_fd_open,             "fd.open");
COMMAND(ctl_cmd_exit,                "exit");
COMMAND(ctl_cmd_log_filter,          "log.filter");
COMMAND(ctl_cmd_event_pipe_timeout,  "conn.event_timeout");
COMMAND(ctl_cmd_terminate_exec_args, "terminate.exec_args");
COMMAND(ctl_cmd_terminate_guard,     "terminate.guard");
COMMAND(ctl_cmd_terminate,           "terminate");

static bool ctl_read_more(controller_t *ctl);
static bool ctl_flush_outbuf(controller_t *ctl);
static bool ctl_out_buf_ready(controller_t *ctl);

static inline bool ctl_peek_arg(controller_t *ctl, strseg_t *arg_out) {
	strseg_t str= ctl->command;
	return strseg_tok_next(&str, '\t', arg_out);
}

static inline bool ctl_get_arg(controller_t *ctl, strseg_t *arg_out) {
	if (!strseg_tok_next(&ctl->command, '\t', arg_out)) {
		ctl->command_error= "missing argument";
		return false;
	}
	return true;
}

static bool ctl_get_arg_int(controller_t *ctl, int64_t *val);
//static bool ctl_get_arg_name_val(controller_t *ctl, strseg_t *name, strseg_t *val);
static bool ctl_get_arg_service(controller_t *ctl, bool existing, strseg_t *name_out, service_t **svc_out);
static bool ctl_get_arg_fd(controller_t *ctl, bool existing, bool assignable, strseg_t *name_out, fd_t **fd_out);
static bool ctl_get_arg_signal(controller_t *ctl, int *sig_out);

typedef struct ctl_command_table_entry_s {
	strseg_t command;
	ctl_state_fn_t *fn;
} ctl_command_table_entry_t;

#include "controller_data.autogen.c"

const ctl_command_table_entry_t * ctl_find_command(strseg_t name) {
	const ctl_command_table_entry_t *result= &ctl_command_table[ ctl_command_hash_func(name) ];
	return result->fn && 0 == strseg_cmp(name, result->command)? result : NULL;
}

void ctl_set_auto_final_newline(controller_t *ctl, bool enable) {
	ctl->append_final_newline= enable;
}

void ctl_init() {
	int i;
	assert(CONTROLLER_MAX_CLIENTS >= 2);
	for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++)
		client[i].state_fn= NULL;
}

controller_t *ctl_new(int recv_fd, int send_fd) {
	controller_t *ctl= ctl_alloc();
	if (!ctl) return NULL;
	if (ctl_ctor(ctl, recv_fd, send_fd))
		return ctl;
	ctl_state_free(ctl);
	return NULL;
}

controller_t *ctl_alloc() {
	controller_t *ctl;
	for (ctl= client; ctl < client + CONTROLLER_MAX_CLIENTS; ctl++)
		if (!ctl->state_fn) {
			// non-null state marks it as allocated
			memset(ctl, 0, sizeof(controller_t));
			ctl->state_fn= &ctl_state_free;
			return ctl;
		}
	return NULL;
}

bool ctl_ctor(controller_t *ctl, int recv_fd, int send_fd) {
	log_debug("creating client %d with handles %d,%d", ctl - client, recv_fd, send_fd);
	// file descriptors must be nonblocking
	if (recv_fd != -1)
		if (fcntl(recv_fd, F_SETFL, O_NONBLOCK)) {
			log_error("fcntl(O_NONBLOCK): errno = %s (%d)", strerror(errno), errno);
			return false;
		}
	if (send_fd != -1 && send_fd != recv_fd)
		if (fcntl(send_fd, F_SETFL, O_NONBLOCK)) {
			log_error("fcntl(O_NONBLOCK): errno = %s (%d)", strerror(errno), errno);
			return false;
		}
	// initialize object
	ctl->state_fn= &ctl_state_next_command;
	ctl->recv_fd= recv_fd;
	ctl->send_fd= send_fd;
	ctl->write_timeout_reset= CONTROLLER_WRITE_TIMEOUT>>1;
	ctl->write_timeout_close= CONTROLLER_WRITE_TIMEOUT;
	return true;
}

void ctl_dtor(controller_t *ctl) {
	log_debug("destroying client %d", ctl - client);
	if (ctl->recv_fd >= 0) close(ctl->recv_fd);
	if (ctl->send_fd >= 0) close(ctl->send_fd);
	ctl->state_fn= ctl_state_free;
}

void ctl_free(controller_t *ctl) {
	ctl->state_fn= NULL;
}

/** Run all processing needed for the controller for this time slice
 * This function is mainly a wrapper that repeatedly executes the current state until
 * the state_fn returns false.  We then flush buffers and decide what to wake on.
 */
void ctl_run(wake_t *wake) {
	int i, j;
	controller_t *ctl;
	ctl_state_fn_t *prev_state;
	int64_t lateness, next_check_ts;
	
	// list is very small, so just iterate all, allocated or not.
	for (i= 0, ctl= client; i < CONTROLLER_MAX_CLIENTS; ctl= &client[++i]) {
		// non-null state means client is allocated
		if (!ctl->state_fn)
			continue;

		// if input buffer not full, try reading more (in case script is blocking on output)
		if (ctl->recv_fd >= 0 && ctl->recv_buf_pos < CONTROLLER_RECV_BUF_SIZE)
			ctl_read_more(ctl);
		
		// Run (max 10) iterations of state machine while state returns true.
		// The arbitrary limit of 10 helps keep our timestamps and signal
		// delivery and reaped procs current.  (also mitigates infinite loops)
		prev_state= NULL;
		for (j= 10; --j;) {
			if (ctl->state_fn != prev_state) {
				log_trace("ctl state = %s", ctl_get_state_name(ctl->state_fn));
				prev_state= ctl->state_fn;
			}
			if (!ctl->state_fn(ctl))
				break;
		}
		// Note: it is possible for ctl to have been destroyed (null state_fn), here.
		
		// If final iteration is still true, set wake to 'now', causing another
		// iteration in the main loop.
		if (!j) wake->next= wake->now;
	}
	
	// Now that all processing is complete for this iteration, flush all output
	// buffers.  This is a separate loop because sometimes controllers generate
	// output to another controller.
	for (i= 0, ctl= client; i < CONTROLLER_MAX_CLIENTS; ctl= &client[++i]) {
		// non-null state means client is allocated
		if (!ctl->state_fn)
			continue;
		
		// If anything was left un-written, wake on writable pipe
		// Also, set/check timeout for writes
		if (ctl->send_fd >= 0 && ctl->send_buf_pos > 0) {
			if (!ctl_flush_outbuf(ctl)) {
				lateness= wake->now - ctl->send_blocked_ts;
				
				// If the controller script doesn't read its events before timeout,
				// close the connection.
				if (lateness >= ctl->write_timeout_close) {
					log_error("controller %d blocked pipe for %d seconds, closing connection", i, (int)(lateness>>32));
					ctl_dtor(ctl); // destroy client
					ctl_free(ctl);
					continue;      // next client
				}
				
				// If the controller script doesn't read events before half its timeout,
				// log a warning, and if the input buffer is full, set an overflow on the
				// output stream so that we can resume processing the commands in the
				// input buffer.  The controller script will have to re-sync state if it
				// finally wakes up.
				if (lateness >= ctl->write_timeout_reset) {
					log_warn("controller %d blocked pipe for %d seconds", i, (int)(lateness>>32));
					if (ctl->recv_buf_pos >= CONTROLLER_RECV_BUF_SIZE) {
						ctl->send_overflow= true;
						wake->next= wake->now;
					}
					next_check_ts= ctl->send_blocked_ts + ctl->write_timeout_close;
				}
				else {
					next_check_ts= ctl->send_blocked_ts + ctl->write_timeout_reset;
				}
				
				if (wake->next - next_check_ts > 0) {
					log_trace("wake in %d ms to check timeout", (next_check_ts - wake->now) * 1000 >> 32 );
					wake->next= next_check_ts;
				}
				log_trace("wake on controller[%d] send_fd", i);
				FD_SET(ctl->send_fd, &wake->fd_write);
				FD_SET(ctl->send_fd, &wake->fd_err);
				if (ctl->send_fd > wake->max_fd)
					wake->max_fd= ctl->send_fd;
			}
			else if (ctl->recv_buf_pos)
				// we might have flushed data that was blocking us from processing
				// additional commands, so come back for another iteration.
				wake->next= wake->now;
		}
		// If incoming fd, wake on data available, unless input buffer full
		// (this could also be the config file, initially)
		if (ctl->recv_fd >= 0 && ctl->recv_buf_pos < CONTROLLER_RECV_BUF_SIZE) {
			log_trace("wake on controller[%d] recv_fd", i);
			FD_SET(ctl->recv_fd, &wake->fd_read);
			FD_SET(ctl->recv_fd, &wake->fd_err);
			if (ctl->recv_fd > wake->max_fd)
				wake->max_fd= ctl->recv_fd;
		}
	}
}

bool ctl_deliver_signals(controller_t *ctl) {
	int signum, sig_count;
	int64_t sig_ts;
	
	while (sig_get_new_events(ctl->last_signal_ts, &signum, &sig_ts, &sig_count)) {
		if (!ctl_out_buf_ready(ctl))
			return false;
		// deliver next signal that this controller hasn't seen
		ctl_notify_signal(ctl, signum, sig_ts, sig_count);
		ctl->last_signal_ts= sig_ts;
	}
	return true;
}

/** Find the next command in the input buffer, if any.
 *
 * This function also handles flagging errors on lines longer than the buffer.
 */
bool ctl_state_next_command(controller_t *ctl) {
	char *eol;
	
	// Before checking commands, deliver any pending signal events
	if (!ctl_deliver_signals(ctl))
		return false; // false means the output buffer is blocked

	// see if we have a full line in the input.  else read some more.
	eol= (char*) memchr(ctl->recv_buf, '\n', ctl->recv_buf_pos);
	if (!eol && ctl->recv_fd >= 0) {
		// if buffer is full, then command is too big, and we ignore the rest of the line
		if (ctl->recv_buf_pos >= CONTROLLER_RECV_BUF_SIZE) {
			// In case its a comment, preserve comment character (long comments are not an error)
			ctl->recv_overflow= true;
			ctl->recv_buf_pos= 1;
			log_trace("line overflows buffer, ignoring remainder");
			return true;
		}
		log_trace("no command ready");
		// done for now, until more data available on pipe
		return false;
	}
	// check for EOF (and append newline if needed)
	if (ctl->recv_fd < 0) {
		// If EOF and nothing in buffer, we're done
		if (ctl->recv_buf_pos == 0) {
			ctl->state_fn= ctl_state_close;
			return true;
		}
		// If last line missing newline, append newline and process command
		// We know we can append because we would never detect EOF unless we read()
		//  and we never read unless there is buffer space.
		if (ctl->recv_buf[ctl->recv_buf_pos-1] != '\n' && ctl->append_final_newline) {
			log_warn("Command ends with EOF... processing anyway");
			ctl->recv_buf[ctl->recv_buf_pos]= '\n';
			eol= ctl->recv_buf + ctl->recv_buf_pos++;
		}
		// if no eol, then ignore final partial command
		if (!eol) {
			log_warn("Command ends with EOF... ignored");
			ctl->recv_buf_pos= 0;
			ctl->state_fn= ctl_state_close;
			return true;
		}
	}

	// We now have a complete line
	ctl->line_len= eol - ctl->recv_buf + 1;
	*eol= '\0';
	log_debug("controller got line: \"%s\"", ctl->recv_buf);
	ctl->state_fn= ctl_state_run_command;
	return true;
}

bool ctl_state_run_command(controller_t *ctl) {
	const ctl_command_table_entry_t *cmd;

	// Commands often generate output, so stop if output buffer too full.
	// For a true solution to the problem, we could add states to each place
	// where we try to write data (like co-routines) and resume a long write().
	// But it saves tons of code if we just make sure we have enough buffer
	// for the largest write we might do.
	if (!ctl_out_buf_ready(ctl))
		return false;
	
	// The next state will be 'end', unless a command changes that
	// (for example, the 'statedump' command)
	ctl->state_fn= ctl_state_end_command;
	
	// Ignore overflow lines, empty lines, lines starting with #, and lines that start with whitespace
	if (ctl->recv_buf[0] == '\n'
		|| ctl->recv_buf[0] == '\r'
		|| ctl->recv_buf[0] == '#'
		|| ctl->recv_buf[0] == ' '
		|| ctl->recv_buf[0] == '\t'
	) {
		log_trace("Ignoring comment line");
		ctl->recv_overflow= false;
	}
	// check for command overflow
	else if (ctl->recv_overflow)
		ctl_notify_error(ctl, "line too long");
	// else try to parse and dispatch it
	else {
		// ctl->command is the un-parsed portion of our command.
		ctl->command.data= ctl->recv_buf;
		ctl->command.len= ctl->line_len - 1; // line_len includes terminating NUL
		ctl->command_error= "unknown error";
		
		// We first parse the command name
		if (strseg_tok_next(&ctl->command, '\t', &ctl->command_name)) {
			// look up the command to see if it exists
			cmd= ctl_find_command(ctl->command_name);
			if (!cmd)
				ctl_notify_error(ctl, "Unknown command: %.*s", ctl->command_name.len, ctl->command_name.data);
			// dispatch it (returns false if it encounters an error, and sets ctl->command_error)
			else if (!cmd->fn(ctl))
				ctl_notify_error(ctl, "%s, for command \"%.*s%s\"", ctl->command_error, ctl->line_len > 30? 30 : ctl->line_len, ctl->recv_buf, ctl->line_len > 30? "...":"");
		}
	}
	return true;
}

bool ctl_state_end_command(controller_t *ctl) {
	// clean up old command, if any.
	if (ctl->line_len > 0) {
		ctl->recv_buf_pos -= ctl->line_len;
		memmove(ctl->recv_buf, ctl->recv_buf + ctl->line_len, ctl->recv_buf_pos);
		ctl->line_len= 0;
	}
	ctl->state_fn= ctl_state_next_command;
	return true;
}

bool ctl_state_close(controller_t *ctl) {
	if (ctl->send_fd >= 0)
		if (!ctl_flush_outbuf(ctl))
			return false; // remain in this state and try again later
	ctl_dtor(ctl);
	return true; // final state is 'free'.
}

bool ctl_state_free(controller_t *ctl) {
	ctl_free(ctl);
	return false; // don't run any more state iterations, because there aren't any
}

/*----------------------------------------------------------------------------

=head1 PROTOCOL

Daemonproxy reads commands in tab-separated-values format, with one
command per line.  There is no escaping mechanism, and your commands
must not contain ASCII control characters.  Events are delivered in
this same format.

In practice, ascii control characters shouldn't be needed, and the
absence of quoting/escaping makes the protocol easier to implement
in your script.

A full protocol reference can be found in the documentation included
with daemonproxy.  However, here is a quick reference guide:

=head2 COMMANDS

=over 4

=item echo ANY_STRING_OF_CHARACTERS

Prints all arguments as-is back as an event.  This is primarily intended to be
used to mark the ends of other commands, by following the other command with
an echo and a unique string, then watching for the echo to complete.

=cut
*/
bool ctl_cmd_echo(controller_t *ctl) {
	if (ctl->command.len > 0)
		ctl_write(ctl, "%.*s\n", ctl->command.len, ctl->command.data);
	return true;
}

/*
=item conn.event_timeout RESET_TIMEOUT CLOSE_TIMEOUT

Set the timeouts associated with this controller.  If the controller's event
stream has been blocked for more than RESET_TIMEOUT seconds, daemonproxy
will flag the connection as "overflowed" and discard further writes until
the script resumes reading events.  If the pipe has not been cleared by
CLOSE_TIMEOUT seconds, daemonproxy will close the pipe and retsart the
controller.

=cut
*/
bool ctl_cmd_event_pipe_timeout(controller_t *ctl) {
	int64_t reset_timeout, close_timeout;
	
	if (!ctl_get_arg_int(ctl, &reset_timeout)
		|| !ctl_get_arg_int(ctl, &close_timeout))
		return false;
	
	if (reset_timeout > 0x7FFFFFFF || reset_timeout < 0
		|| close_timeout > 0x7FFFFFFF || close_timeout < 0)
	{
		ctl->command_error= "invalid timeout (must be 0..7FFFFFFF)";
		return false;
	}
	if (reset_timeout > close_timeout) {
		ctl->command_error= "reset timeout is greater than close timeout";
		return false;
	}
	
	ctl->write_timeout_reset= (reset_timeout << 32);
	ctl->write_timeout_close= (close_timeout << 32);
	return true;
}

/*
=item exit

Close the connection to daemonproxy.  'exit' is a poor name for this command,
but people expect to be able to type 'exit' to end a command stream.

=cut
*/
bool ctl_cmd_exit(controller_t *ctl) {
	// they asked for it...
	if (ctl->recv_fd >= 0) close(ctl->recv_fd);
	ctl->recv_fd= -1;
	ctl->state_fn= ctl_state_close;
	return true;
}

/*
=item statedump

Re-emit all events for daemonproxy's current state, to get the controller back
into sync.  Useful after event overflow, or controller restart.

=cut
*/
bool ctl_cmd_statedump(controller_t *ctl) {
	ctl->state_fn= ctl_state_dump_fds;
	ctl->statedump_current[0]= '\0';
	ctl->command_substate= 0;
	return true;
}

bool ctl_state_dump_fds(controller_t *ctl) {
	fd_t *fd= fd_by_name(STRSEG(ctl->statedump_current));
	if (!fd) ctl->command_substate= 0;
	/* Statedump command, part 1: iterate fd objects and dump each one.
	 * Uses an iterator of fd_t's name so that if fds are creeated or destroyed during our
	 * iteration, we can safely resume.  (and this iteration could be idle for a while if
	 * the controller script doesn't read its pipe)
	 */
 switch (ctl->command_substate) {
 case 0:
	
	while ((fd= fd_iter_next(fd, ctl->statedump_current))) {
		log_trace("fd iter = %s", fd_get_name(fd));
 case 1:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 1; break; }
		ctl_notify_fd_state(ctl, fd);
	}
 } //switch
	if (fd) { // If we broke the loop early, record name of where to resume
		strcpy(ctl->statedump_current, fd_get_name(fd)); // length of name has already been checked
		return false;
	}
	ctl->statedump_current[0]= '\0';
	ctl->state_fn= ctl_state_dump_services;
	ctl->command_substate= 0;
	return true;
}

bool ctl_state_dump_services(controller_t *ctl) {
	service_t *svc= svc_by_name(STRSEG(ctl->statedump_current), false);
	if (!svc) ctl->command_substate= 0;
	/* Statedump command, part 2: iterate services and dump each one.
	 * Like part 1 above, except a service has 4 lines of output.
	 */
 switch (ctl->command_substate) {
 case 0:

	while ((svc= svc_iter_next(svc, ctl->statedump_current))) {
		log_trace("service iter = %s", svc_get_name(svc));
 case 1:
		svc_check(svc);
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 1; break; }
		ctl_notify_svc_state(ctl, svc_get_name(svc), svc_get_up_ts(svc),
			svc_get_reap_ts(svc), svc_get_pid(svc), svc_get_wstat(svc));
// case 2:
//		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 2; break; }
//		ctl_notify_svc_meta(ctl, svc_get_name(svc), svc_get_meta(svc));
 case 3:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 3; break; }
		ctl_notify_svc_argv(ctl, svc_get_name(svc), svc_get_argv(svc));
 case 4:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 4; break; }
		ctl_notify_svc_fds(ctl, svc_get_name(svc), svc_get_fds(svc));
	}
 }//switch
	if (svc) { // If we broke the loop early, record name of where to resume
		strcpy(ctl->statedump_current, svc_get_name(svc)); // length of name has already been checked
		return false;
	}
	ctl->last_signal_ts= 0;
	ctl->state_fn= ctl_state_dump_signals;
	ctl->command_substate= 0;
	return true;
}

bool ctl_state_dump_signals(controller_t *ctl) {
	// Just use the deliver_signals, after having reset the last_signal_ts
	if (!ctl_deliver_signals(ctl))
		return false;
	// But, need to override the state transition that it performs when complete
	ctl->state_fn= ctl_state_end_command;
	return true;
}

/*
=item fd.pipe NAME_READ NAME_WRITE

Create a pipe, with the read-end named NAME_READ and write-end named
NAME_WRITE.  Re-using an existing name will close the old handle.

=cut
*/
bool ctl_cmd_fd_pipe(controller_t *ctl) {
	fd_t *fd;
	int pair[2];
	strseg_t read_side, write_side;
	
	if (!ctl_get_arg_fd(ctl, false, true, &read_side, NULL)
		|| !ctl_get_arg_fd(ctl, false, true, &write_side, NULL))
		return false;
	
	if (0 != pipe(pair)) {
		ctl_notify_error(ctl, );
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"pipe() failed: %s", strerror(errno));
		ctl->command_error= ctl->command_error_buf;
	}
	
	fd= fd_new_pipe(read_side, pair[0], write_side, pair[1]);
	if (!fd) {
		close(pair[0]);
		close(pair[1]);
		ctl->command_error= "failed to create pipe";
		return false;
	}
	
	ctl_notify_fd_state(NULL, fd);
	ctl_notify_fd_state(NULL, fd_get_pipe_peer(fd));
	return true;
}

/*
=item fd.open NAME FLAG1,FLAG2,.. PATH

Opens a file at PATH.  FLAGS is a comma-sparated list of flags of the
set read, write, create, truncate, nonblock, mkdir.  Re-using an
existing name will close the old handle.

=cut
*/
bool ctl_cmd_fd_open(controller_t *ctl) {
	int f, open_flags;
	fd_flags_t flags;
	fd_t *fd;
	strseg_t fdname, opts, opt, path;

	if (!ctl_get_arg_fd(ctl, false, true, &fdname, NULL))
		return false;
	
	if (!ctl_get_arg(ctl, &opts)) {
		ctl->command_error= "missing flags argument";
		return false;
	}
	
	if (!ctl_get_arg(ctl, &path)) {
		ctl->command_error= "missing path argument";
		return false;
	}
	
	if (ctl_peek_arg(ctl, NULL)) {
		ctl->command_error= "unexpected argument after path";
		return false;
	}
	assert(path.data[path.len] == '\0');
	
	memset(&flags, 0, sizeof(flags));
	#define STRMATCH(flag) (opt.len == strlen(flag) && 0 == memcmp(opt.data, flag, opt.len))
	while (strseg_tok_next(&opts, ',', &opt)) {
		switch (opt.data[0]) {
		case 'a':
			if (STRMATCH("append")) { flags.append= true; continue; }
			break;
		case 'c':
			if (STRMATCH("create")) { flags.create= true; continue; }
			break;
		case 'm':
			if (STRMATCH("mkdir")) { flags.mkdir= true; continue; }
			break;
		case 'r':
			if (STRMATCH("read")) { flags.read= true; continue; }
			break;
		case 't':
			if (STRMATCH("trunc")) { flags.trunc= true; continue; }
			break;
		case 'w':
			if (STRMATCH("write")) { flags.write= true; continue; }
			break;
		case 'n':
			if (STRMATCH("nonblock")) { flags.nonblock= true; continue; }
			break;
		}
		
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"unknown flag \"%.*s\"\n", opt.len, opt.data);
		ctl->command_error= ctl->command_error_buf;
		return false;
	}
	#undef STRMATCH
	
	if (flags.mkdir)
		// we don't check success on this.  we just let open() fail and check that.
		create_missing_dirs((char*)path.data);

	open_flags= (flags.write? (flags.read? O_RDWR : O_WRONLY) : O_RDONLY)
		| (flags.append? O_APPEND : 0) | (flags.create? O_CREAT : 0)
		| (flags.trunc?  O_TRUNC  : 0) | (flags.nonblock? O_NONBLOCK : 0)
		| O_NOCTTY;

	f= open(path.data, open_flags, 0600);
	if (f < 0) {
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"open failed: %s", strerror(errno));
		ctl->command_error= ctl->command_error_buf;
		return false;
	}

	fd= fd_new_file(fdname, f, flags, path);
	if (!fd) {
		close(f);
		ctl->command_error= "no room for new fd object";
		return false;
	}

	ctl_notify_fd_state(NULL, fd);
	return true;
}

/*
=item fd.delete NAME

Close (and remove) the named handle.

=cut
*/


/*
=item service.args NAME ARG_1 ARG_2 ... ARG_N

Assign new exec() arguments to the service.  NAME will be created if
it didn't exist.  ARG_1 is both the file to execute and argv[0] to
pass to the service.  To falsify argv[0], use an external program.

=cut
*/
bool ctl_cmd_svc_args(controller_t *ctl) {
	service_t *svc;
	
	if (!ctl_get_arg_service(ctl, false, NULL, &svc))
		return false;
	
	if (!svc_set_argv(svc, ctl->command.len >= 0? ctl->command : STRSEG(""))) {
		ctl->command_error= "unable to set argv";
		return false;
	}
	
	ctl_notify_svc_argv(NULL, svc_get_name(svc), svc_get_argv(svc));
	return true;
}

/*
=item service.fds NAME HANDLE_1 HANDLE_2 ... HANDLE_N

Set the list of file descriptors to pass to the service.  Name will
be created if it didn't exist.  The name 'null' is always available
and refers to /dev/null.  '-' means to pass the service a closed
file descriptor.

=cut
*/
bool ctl_cmd_svc_fds(controller_t *ctl) {
	service_t *svc;
	strseg_t fd_spec, name;
	
	if (!ctl_get_arg_service(ctl, false, NULL, &svc))
		return false;
	
	fd_spec= ctl->command;
	while (strseg_tok_next(&ctl->command, '\t', &name)) {
		if (!fd_check_name(name)) {
			ctl->command_error= "invalid fd name";
			return false;
		}
		if (!fd_by_name(name))
			ctl_write(ctl, "warning: fd \"%.*s\" is not yet defined\n", name.len, name.data);
	}
	
	if (!svc_set_fds(svc, fd_spec)) {
		ctl->command_error= "unable to set file descriptors";
		return false;
	}
	
	ctl_notify_svc_fds(NULL, svc_get_name(svc), svc_get_fds(svc));
	return true;
}

/*
=item service.start NAME [FUTURE_TIMESTAMP]

Start the service, optionally at a future time.  Errors in service specification
are reported immediate.  Errors during fork/exec are reported later.

=cut
*/
bool ctl_cmd_svc_start(controller_t *ctl) {
	const char *argv;
	int64_t starttime_ts;
	service_t *svc;
	
	if (!ctl_get_arg_service(ctl, true, NULL, &svc))
		return false;
	
	// Optional timestamp for service start
	if (ctl_peek_arg(ctl, NULL)) {
		if (!ctl_get_arg_int(ctl, &starttime_ts) || starttime_ts - wake->now < 10000) {
			ctl->command_error= "invalid timestamp";
			return false;
		}
	}
	else starttime_ts= wake->now;
	
	argv= svc_get_argv(svc);
	if (!argv[0] || argv[0] == '\t') {
		ctl->command_error= "no args configured for service";
		return false;
	}
	
	svc_handle_start(svc, starttime_ts);
	return true;
}

/*
=item service.signal NAME SIGNAL [FLAGS]

Send SIGNAL to the named service's pid, if it is running.  Optional flag may
be "group", in which case (if the service leads a process group) the pprocess
group is sent the signal.

=cut
*/
bool ctl_cmd_svc_signal(controller_t *ctl) {
	service_t *svc;
	bool group= false;
	strseg_t flags, flag;
	int sig;
	
	if (!ctl_get_arg_service(ctl, true, NULL, &svc))
		return false;
	
	if (!ctl_get_arg_signal(ctl, &sig))
		return false;
	
	if (ctl_peek_arg(ctl, &flags)) {
		while (strseg_tok_next(&flags, ',', &flag)) {
			if (strseg_cmp(flag, STRSEG("group")) == 0)
				group= true;
			else {
				snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
					"unknown option \"%.*s\"", flag.len, flag.data);
				ctl->command_error= ctl->command_error_buf;
				return false;
			}
		}
	}
	
	if (svc_get_pid(svc) <= 0 || svc_get_wstat(svc) >= 0) {
		ctl->command_error= "service is not running";
		return false;
	}
	
	if (!svc_send_signal(svc, sig, group)) {
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"can't kill %s (%s %d): %s", svc_get_name(svc), group? "pgid":"pid", (int)svc_get_pid(svc), strerror(errno));
		ctl->command_error= ctl->command_error_buf;
		return false;
	}
	return true;
}

/*
=item log.filter [+|-|none|LEVELNAME]

Change the logging filter level of daemonproxy.  A value of none causes all
log messages to be printed.  A value of + or - increases or decreases the
filter level.  A level of 'info' would suppress 'info', 'debug', and 'trace'
messages.  Note that trace messages are only available when compiled in
debug mode.

=cut
*/
bool ctl_cmd_log_filter(controller_t *ctl) {
	strseg_t arg;
	int level;
	
	// Optional argument to set the filter level, else just print it
	if (ctl_peek_arg(ctl, &arg)) {
		// Level can be a level name, or "+" or "-"
		if (arg.len == 1 && arg.data[0] == '+')
			level= log_filter + 1;
		else if (arg.len == 1 && arg.data[0] == '-')
			level= log_filter - 1;
		else if (!log_level_by_name(arg, &level)) {
			ctl->command_error= "Invalid loglevel argument";
			return false;
		}
		// If got a level, assign it.
		log_set_filter(level);
	}

	ctl_write(ctl, "log.filter\t%s\n", log_level_name(log_filter) );
	return true;
}

/*
=item terminate [EXIT_CODE] [GUARD_CODE]

Terminate daemonproxy immediately.  No cleanup is performed, and all handles
and child processes will be lost.  Graceful shutdown should be part of the
controller script, and this should be the final step.

If the terminate-guard feature is enabled, then you need an additional argument
of the correct code in order for the command to happen.

If the exec-on-exit feature is enabled, daemonproxy will exec() instead
of exit().  If daemonproxy is process 1, terminate will fail unless
exec-on-exit is enabled.

=cut
*/
bool ctl_cmd_terminate(controller_t *ctl) {
	int exitcode;
	int64_t x;

	// First argument is exit value
	if (!ctl_get_arg_int(ctl, &x))
		return false;
	exitcode= (int) x;

	// Next argument is optional guard code
	if (main_terminate_guard) {
		if (!ctl_get_arg_int(ctl, &x)) {
			ctl->command_error= "terminate guard code required";
			return false;
		}
		if (main_terminate_guard != x) {
			ctl->command_error= "incorrect terminate guard code";
			return false;
		}
	}
	
	// Can't exit if running as pid 1
	if (main_terminate_guard && !main_exec_on_exit) {
		ctl->command_error= "cannot exit, and exec-on-exit is not configured";
		return false;
	}
	
	main_terminate= true;
	main_exitcode= exitcode;
	wake->next= wake->now;
	return true;
}

/*
=item terminate.exec_args [ARG_1] .. [ARG_N]

Set argument list for daemonproxy's exec-on-exit feature.  This feature causes
daemonproxy to exec(ARGS) instead of exiting in any trappable scenario.  An
empty argument list disables the feature.

=cut
*/
bool ctl_cmd_terminate_exec_args(controller_t *ctl) {
	if (!set_exec_on_exit(ctl->command)) {
		ctl->command_error= "exec arguments exceed buffer size (255)";
		return false;
	}
	return true;
}

/*
=item terminate.guard [+|-] CODE

Enable or disable the terminate-guard feature.  '+' enables the feature and
sets the guard code to CODE.  '-' disables the feature only if CODE matches
the previously set value.

=cut
*/
bool ctl_cmd_terminate_guard(controller_t *ctl) {
	strseg_t arg;
	int64_t code;
	
	if (!ctl_get_arg(ctl, &arg) || arg.len != 1
		|| (arg.data[0] != '-' && arg.data[0] != '+'))
	{
		ctl->command_error= "expected + or -";
		return false;
	}

	if (!ctl_get_arg_int(ctl, &code))
		return false;
	
	if (!code) {
		ctl->command_error= "code cannot be 0";
		return false;
	}
	
	if (arg.data[0] == '+') {
		if (main_terminate_guard) {
			ctl->command_error= "terminate guard code is already set";
			return false;
		}
		main_terminate_guard= code;
		return true;
	}
	else {
		if (!main_terminate_guard) {
			ctl->command_error= "terminate guard was not set";
			return false;
		}
		if (code != main_terminate_guard) {
			ctl->command_error= "incorrect guard code";
			return false;
		}
		main_terminate_guard= 0;
		return true;
	}

	ctl_write(ctl, "log.filter\t%s\n", log_level_name(log_filter) );
	return true;
}

/*-----------------------------------------------------------------------------
 * end of commands

=back

=head2 EVENTS

=over 4

=item signal NAME TS COUNT

NAME is the C constant like SIGINT.  TS is a timestamp from CLOCK_MONOTONIC
when the signal was last received.  COUNT is the number of times it was
received since last cleared (however you can't actually know the exact count
due to the nature of signals)

=cut
*/
bool ctl_notify_signal(controller_t *ctl, int sig_num, int64_t sig_ts, int count) {
	const char *signame= sig_name_by_num(sig_num);
	return ctl_write(NULL, "signal	SIG%s	%d	%d\n", signame? signame : "-?", (int)(sig_ts>>32), count);
}

/*
=item service.state NAME STATE TS PID EXITREASON EXITVALUE UPTIME DOWNTIME

The state of service has changed.  STATE is 'start', 'up', 'down', or 'deleted'.
TS is a timestamp from CLOCK_MONOTONIC.  PID is the process ID if relevant,
and '-' otherwise.  EXITREASON is '-', 'exit', or 'signal'.  EXITVALUE is an
integer or signal name.  UPTIME and DOWNTIME are in seconds, and '-' if not
relevant.

=cut
*/

bool ctl_notify_svc_state(controller_t *ctl, const char *name, int64_t up_ts, int64_t reap_ts, int wstat, pid_t pid) {
	const char *signame;
	log_trace("ctl_notify_svc_state(%s, %lld, %lld, %d, %d)", name, up_ts, reap_ts, pid, wstat);
	if (!up_ts)
		return ctl_write(ctl, "service.state	%s	down	-	-	-	-	-	-\n", name);
	else if ((up_ts - wake->now) >= 0 && !pid)
		return ctl_write(ctl, "service.state	%s	start	%d	-	-	-	-	-\n",
			name, (int)(up_ts>>32));
	else if (!reap_ts)
		return ctl_write(ctl, "service.state	%s	up	%d	%d	-	-	%d	-\n",
			name, (int)(up_ts>>32), (int) pid, (int)((wake->now - up_ts)>>32));
	else if (WIFEXITED(wstat))
		return ctl_write(ctl, "service.state	%s	down	%d	%d	exit	%d	%d	%d\n",
			name, (int)(reap_ts>>32), (int) pid, WEXITSTATUS(wstat),
			(int)((reap_ts - up_ts)>>32), (int)((wake->now - reap_ts)>>32));
	else {
		signame= sig_name_by_num(WTERMSIG(wstat));
		return ctl_write(ctl, "service.state	%s	down	%d	%d	signal	SIG%s	%d	%d\n",
			name, (int)(reap_ts>>32), (int) pid, signame? signame : "-?",
			(int)((reap_ts - up_ts)>>32), (int)((wake->now - reap_ts)>>32));
	}
}

/*
=item service.args NAME ARG_1 ARG_2 ... ARG_N

Arguments for the service have changed

=cut
*/
bool ctl_notify_svc_argv(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.args	%s	%s\n", name, tsv_fields);
}

/*
=item service.fds NAME HANDLE_1 HANDLE_2 ... HANDLE_N

File handles for the service have changed

=cut
*/
bool ctl_notify_svc_fds(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.fds	%s	%s\n", name, tsv_fields);
}

/*
=item fd.state NAME TYPE FLAGS DESCRIPTION

TYPE is 'file', 'pipe', 'special', or 'deleted'.  Deleted means the file
handle has just been removed and no longer exists.  Type 'file' has FLAGS
that match the flags used to open it (though possibly in a different order).
Type 'pipe' has flags of 'to' or 'from'.  DESCRIPTION is the filename
(possibly truncated), the pipe-peer handle name, or a free-form string
describing the handle.

=cut
*/
bool ctl_notify_fd_state(controller_t *ctl, fd_t *fd) {
	fd_t *peer;
	const char *name= fd_get_name(fd);
	fd_flags_t flags= fd_get_flags(fd);
	
	if (flags.pipe) {
		peer= fd_get_pipe_peer(fd);
		return ctl_write(ctl, "fd.state	%s	pipe	%s	%s\n",
			name, flags.write? "to":"from", peer? fd_get_name(peer) : "?");
	}
	else {
		return ctl_write(ctl, "fd.state	%s	%s	%s%s%s%s%s%s	%s\n",
			name, flags.special? "special" : "file",
			(flags.write? (flags.read? "read,write":"write"):"read"),
			(flags.append? ",append":""), (flags.create? ",create":""),
			(flags.trunc? ",trunc":""), (flags.nonblock? ",nonblock":""),
			(flags.mkdir? ",mkdir":""), fd_get_file_path(fd));
	}
}

/*----------------------------------------------------------------------------
 * End of events

=back

=cut
*/

// Read more controller input from recv_fd
bool ctl_read_more(controller_t *ctl) {
	int n, e;
	if (ctl->recv_fd < 0 || ctl->recv_buf_pos >= CONTROLLER_RECV_BUF_SIZE)
		return false;
	n= read(ctl->recv_fd, ctl->recv_buf + ctl->recv_buf_pos, CONTROLLER_RECV_BUF_SIZE - ctl->recv_buf_pos);
	if (n <= 0) {
		e= errno;
		log_trace("controller input read failed: %d %s", n, strerror(e));
		if (n == 0) {
			// EOF.  Close file descriptor
			close(ctl->recv_fd);
			ctl->recv_fd= -1;
		}
		else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			log_error("read(controller pipe): %s", strerror(e));
		errno= e;
		return false;
	}
	ctl->recv_buf_pos += n;
	log_trace("controller read %d bytes (%d in recv buf)", n, ctl->recv_buf_pos);
	return true;
}

// Try to write data to a controller, nonblocking.
// Return true if the message was queued, or false if it can't be written.
// If ctl is NULL, then all controllers with send_fd will be notified, assuming
//  they aren't in an overflow condition.  Return value is always true when broadcasting.
bool ctl_write(controller_t *single_dest, const char *fmt, ... ) {
	controller_t * dest[CONTROLLER_MAX_CLIENTS];
	int dest_n, i;
	
	// Either send one message, or iterate all clients
	if (single_dest) {
		if (!single_dest->state_fn || single_dest->send_fd < 0 || single_dest->send_overflow)
			return true;
		dest[0]= single_dest;
		dest_n= 1;
	}
	else {
		dest_n= 0;
		for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++) {
			if (!client[i].state_fn || client[i].send_fd < 0 || client[i].send_overflow)
				continue;
			dest[dest_n++]= &client[i];
		}
		if (!dest_n)
			return true;
	}
	log_trace("write msg to %d controllers", dest_n);
	
	const char *msg_data= NULL;
	int msg_len= -1;
	int p, buf_free;
	for (i= 0; i < dest_n; i++) {
		check_space:
		buf_free= CONTROLLER_SEND_BUF_SIZE - dest[i]->send_buf_pos;
		// see if message fits in buffer
		if (msg_len >= buf_free) {
			// try flushing
			p= dest[i]->send_buf_pos;
			ctl_flush_outbuf(dest[i]);
			// check if flushing made any progress
			if (p != dest[i]->send_buf_pos)
				goto check_space;
			
			log_debug("can't write msg, %d > buffer free %d", msg_len, buf_free);
			// if it is a broadcast, we mark the client outbuf as having overflowed
			//if (!single_dest)
				dest[i]->send_overflow= true;
		}
		else {
			// If this is the second+ time printing, we can memcpy from the first
			if (msg_data)
				memcpy(dest[i]->send_buf + dest[i]->send_buf_pos, msg_data, msg_len);
			// else printf
			else {
				va_list val;
				va_start(val, fmt);
				msg_len= vsnprintf(
					dest[i]->send_buf + dest[i]->send_buf_pos,
					buf_free,
					fmt, val);
				va_end(val);
				// This might be the first time we ran printf, in which case we haven't really
				// tested msg_len yet.
				if (msg_len >= buf_free) {
					log_trace("first write attempt didn't fit");
					goto check_space;
				}
				msg_data= dest[i]->send_buf + dest[i]->send_buf_pos; // save for next iter
			}
			dest[i]->send_buf_pos += msg_len;
			log_trace("appended %d to out_buf (%d pending)", msg_len, dest[i]->send_buf_pos);
		}
	}
	
	// return true if was a broadcast, or if succeeded
	return true;//!single_dest || msg_data;
}

void create_missing_dirs(char *path) {
	char *end;
	for (end= strchr(path, '/'); end; end= strchr(end+1, '/')) {
		*end= '\0';
		mkdir(path, 0700); // would probably take longer to stat than to just let mkdir fail
		*end= '/';
	}
}

// Try to flush the output buffer (nonblocking)
// Return true if flushed completely.  false otherwise.
static bool ctl_flush_outbuf(controller_t *ctl) {
	int n;
	while (ctl->send_buf_pos) {
		log_trace("controller write buffer %d bytes pending %s",
			ctl->send_buf_pos, ctl->send_overflow? "(overflow flag set)" : "");
		// if no send_fd, discard buffer
		if (ctl->send_fd == -1)
			ctl->send_buf_pos= 0;
		// else write as much as we can (on nonblocking fd)
		else {
			n= write(ctl->send_fd, ctl->send_buf, ctl->send_buf_pos);
			if (n > 0) {
				log_trace("controller flushed %d bytes", n);
				ctl->send_buf_pos -= n;
				ctl->send_blocked_ts= 0;
				memmove(ctl->send_buf, ctl->send_buf + n, ctl->send_buf_pos);
			}
			else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				// mark the time when this happened.  We clear this next time a write succeeds
				// If the write blocks for too long, ctl_run will perform timeout actions
				if (!ctl->send_blocked_ts)
					ctl->send_blocked_ts= wake->now? wake->now : 1; // timestamp must be nonzero
				return false;
			} else {
				// fatal error
				log_debug("controller outbuf write failed: %s", strerror(errno));
				close(ctl->send_fd);
				ctl->send_fd= -1;
				return true;  // the buffer is now "flushed" for all practical purposes
			}
		}
	}
	// If we just finished emptying the buffer, and the overflow flag is set,
	// then send the overflow message.
	if (ctl->send_overflow) {
		memcpy(ctl->send_buf, "overflow\n", 9);
		ctl->send_buf_pos= 9;
		ctl->send_overflow= false;
		return ctl_flush_outbuf(ctl);
	}
	return true;
}

static bool ctl_out_buf_ready(controller_t *ctl) {
	return ctl->send_buf_pos <= (CONTROLLER_SEND_BUF_SIZE-CONTROLLER_LARGEST_WRITE)
		|| ctl->send_overflow // if overflow, just allow writes to be discarded
		|| ctl_flush_outbuf(ctl);
}

/** Extract the next argument as an integer
 */
bool ctl_get_arg_int(controller_t *ctl, int64_t *val) {
	strseg_t str;
	if (!strseg_tok_next(&ctl->command, '\t', &str)
		|| !strseg_atoi(&str, val)
		|| str.len > 0   // should consume all of str
	) {
		ctl->command_error= "Expected integer";
		return false;
	}
	return true;
}

/** Extract the next argument as a name=value pair
 */
//bool ctl_get_arg_name_val(controller_t *ctl, strseg_t *name, strseg_t *val) {
//	strseg_t str, n;
//	if (!strseg_tok_next(&ctl->command, '\t', &str)
//		|| !strseg_tok_next(&str, '=', &n)
//		|| n.len <= 0 || str.len < 0
//	) {
//		ctl->command_error= "Expected name=value pair";
//		return false;
//	}
//	if (name) *name= n;
//	if (val) *val= str;
//	return true;
//}

/** Extract the next argument as a service reference
 */
bool ctl_get_arg_service(controller_t *ctl, bool existing, strseg_t *name_out, service_t **svc_out) {
	strseg_t name;
	service_t *svc;
	
	if (!strseg_tok_next(&ctl->command, '\t', &name) || !name.len) {
		ctl->command_error= "Expected service name";
		return false;
	}
	if (!svc_check_name(name)) {
		ctl->command_error= "Invalid service name";
		return false;
	}
	svc= svc_by_name(name, !existing);
	if (!svc) {
		ctl->command_error= existing? "No such service" : "Unable to create new service";
		return false;
	}
	if (name_out) *name_out= name;
	if (svc_out) *svc_out= svc;
	return true;
}

bool ctl_get_arg_fd(controller_t *ctl, bool existing, bool assignable, strseg_t *name_out, fd_t **fd_out) {
	strseg_t name;
	fd_t *fd;
	
	if (!strseg_tok_next(&ctl->command, '\t', &name) || !name.len) {
		ctl->command_error= "Expected file descriptor name";
		return false;
	}
	if (!fd_check_name(name)) {
		ctl->command_error= "Invalid file descriptor name";
		return false;
	}
	fd= fd_by_name(name);
	if (assignable && fd && fd_get_flags(fd).is_const) {
		ctl->command_error= "File descriptor cannot be altered";
		return false;
	}
	if (existing && !fd) {
		ctl->command_error= "No such file descriptor";
		return false;
	}
	if (name_out) *name_out= name;
	if (fd_out) *fd_out= fd;
	return true;
}

bool ctl_get_arg_signal(controller_t *ctl, int *sig_out) {
	strseg_t signame;
	int64_t i;
	int signum;
	
	if (!strseg_tok_next(&ctl->command, '\t', &signame) || !signame.len) {
		ctl->command_error= "Expected signal argument";
		return false;
	}
	if (signame.data[0] >= '0' && signame.data[0] <= '9') {
		if (!strseg_atoi(&signame, &i) || signame.len != 0 || i < 0 || i >> 16) {
			ctl->command_error= "Invalid signal number";
			return false;
		}
		signum= (int) i;
	}
	else {
		if (0 > (signum= sig_num_by_name(signame))) {
			ctl->command_error= "Invalid signal argument";
			return false;
		}
	}
	if (sig_out) *sig_out= signum;
	return true;
}

