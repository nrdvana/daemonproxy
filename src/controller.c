/* controller.c - routines for parsing commands and sending events
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

struct controller_s;
typedef bool ctl_state_fn_t(struct controller_s *);

struct controller_s {
	ctl_state_fn_t *state_fn;
	int id;
	
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
	char    statedump_current[NAME_BUF_SIZE];
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

// Each of the command functions returns true on success,
// or sets ctl->command_error to an error message and returns false.
// The second argument in this macro is used by the perl script that
// generates the static hash table of commands.

#define COMMAND(name, ...) static bool name(controller_t *ctl)
COMMAND(ctl_cmd_echo,                "echo");
COMMAND(ctl_cmd_statedump,           "statedump");
COMMAND(ctl_cmd_svc_tags,            "service.tags");
COMMAND(ctl_cmd_svc_args,            "service.args");
COMMAND(ctl_cmd_svc_fds,             "service.fds");
COMMAND(ctl_cmd_svc_auto_up,         "service.auto_up");
COMMAND(ctl_cmd_svc_start,           "service.start");
COMMAND(ctl_cmd_svc_signal,          "service.signal");
COMMAND(ctl_cmd_svc_delete,          "service.delete");
COMMAND(ctl_cmd_socket_create,       "socket.create");
COMMAND(ctl_cmd_socket_delete,       "socket.delete");
COMMAND(ctl_cmd_fd_pipe,             "fd.pipe");
COMMAND(ctl_cmd_fd_open,             "fd.open");
COMMAND(ctl_cmd_fd_socket,           "fd.socket");
COMMAND(ctl_cmd_fd_delete,           "fd.delete");
COMMAND(ctl_cmd_chdir,               "chdir");
COMMAND(ctl_cmd_exit,                "exit");
COMMAND(ctl_cmd_log_filter,          "log.filter");
COMMAND(ctl_cmd_log_dest,            "log.dest");
COMMAND(ctl_cmd_event_pipe_timeout,  "conn.event_timeout");
COMMAND(ctl_cmd_signal_clear,        "signal.clear");
COMMAND(ctl_cmd_terminate_exec_args, "terminate.exec_args");
COMMAND(ctl_cmd_terminate_guard,     "terminate.guard");
COMMAND(ctl_cmd_terminate,           "terminate");

static bool ctl_read_more(controller_t *ctl);
static bool ctl_flush_outbuf(controller_t *ctl);
static bool ctl_out_buf_ready(controller_t *ctl);

//
// These "get_arg" functions are convenience for the command implementations,
// and handle all the most common tasks of argument parsing.
//

// Extract next argument into arg_out, but don't remove from the command string.
static inline bool ctl_peek_arg(controller_t *ctl, strseg_t *arg_out) {
	strseg_t str= ctl->command;
	return strseg_tok_next(&str, '\t', arg_out);
}

// Extract argument from command into arg_out
static inline bool ctl_get_arg(controller_t *ctl, strseg_t *arg_out) {
	if (!strseg_tok_next(&ctl->command, '\t', arg_out)) {
		ctl->command_error= "missing argument";
		return false;
	}
	return true;
}

static bool ctl_get_arg_int(controller_t *ctl, int64_t *val);
static bool ctl_get_arg_ts(controller_t *ctl, int64_t *ts);
//static bool ctl_get_arg_name_val(controller_t *ctl, strseg_t *name, strseg_t *val);
static bool ctl_get_arg_service(controller_t *ctl, bool existing, strseg_t *name_out, service_t **svc_out);
static bool ctl_get_arg_fd(controller_t *ctl, bool existing, bool assignable, strseg_t *name_out, fd_t **fd_out);
static bool ctl_get_arg_signal(controller_t *ctl, int *sig_out);

//
// Here we define a static hash table of commands, and methods to access them.
//

typedef struct ctl_command_table_entry_s {
	strseg_t command;
	ctl_state_fn_t *fn;
} ctl_command_table_entry_t;

#include "controller_data.autogen.c"

const ctl_command_table_entry_t * ctl_find_command(strseg_t name) {
	const ctl_command_table_entry_t *result= &ctl_command_table[ ctl_command_hash_func(name) ];
	return result->fn && 0 == strseg_cmp(name, result->command)? result : NULL;
}


// A controller can be set to be less strict, and not require a newline
// right before EOF
void ctl_set_auto_final_newline(controller_t *ctl, bool enable) {
	ctl->append_final_newline= enable;
}

/* Initialize controller subsystem
 *
 * We allocate controller clients out of a small static pool.  In most cases
 * there will only be one client.  (daemonproxy does not do much synchronization
 * between clients; this is the duty of the main controller script, which should
 * be the only client.)
 */
void ctl_init() {
	int i;
	assert(CONTROLLER_MAX_CLIENTS >= 2);
	for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++)
		client[i].state_fn= NULL;
}

/* Allocate/construct next controller and bind it to an input and output handle.
 *
 * Returns NULL if there are no free controllers, or if the constructor fails.
 */
controller_t *ctl_new(int recv_fd, int send_fd) {
	controller_t *ctl= ctl_alloc();
	if (!ctl) return NULL;
	if (ctl_ctor(ctl, recv_fd, send_fd))
		return ctl;
	ctl_state_free(ctl);
	return NULL;
}

/* Allocate the next unused controller from the static pool.
 *
 * Returns false if there are no free objects int he pool.
 */
controller_t *ctl_alloc() {
	int i;

	for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++)
		if (!client[i].state_fn) {
			// non-null state marks it as allocated
			memset(&client[i], 0, sizeof(controller_t));
			client[i].state_fn= &ctl_state_free;
			client[i].id= i;
			return &client[i];
		}
	return NULL;
}

/* Constructor (not including alloc)
 *
 * Initialize and bind a controller object to a pair of in/out handles.
 */
bool ctl_ctor(controller_t *ctl, int recv_fd, int send_fd) {
	log_debug("creating client %d with handles %d,%d", ctl->id, recv_fd, send_fd);
	// file descriptors must be nonblocking
	if (recv_fd != -1)
		if (fcntl(recv_fd, F_SETFL, O_NONBLOCK)) {
			log_error("fcntl(O_NONBLOCK): %s", strerror(errno));
			return false;
		}
	if (send_fd != -1 && send_fd != recv_fd)
		if (fcntl(send_fd, F_SETFL, O_NONBLOCK)) {
			log_error("fcntl(O_NONBLOCK): %s", strerror(errno));
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

/* Destructor (not including free)
 *
 * Finalize the state of a controller object.
 */
void ctl_dtor(controller_t *ctl) {
	log_debug("destroying client %d", ctl->id);
	if (ctl->recv_fd >= 0)
		close(ctl->recv_fd);
	if (ctl->send_fd >= 0 && ctl->send_fd != ctl->recv_fd)
		close(ctl->send_fd);
	ctl->state_fn= ctl_state_free;
}

/* Free a controller object, by returning it to the pool.
 */
void ctl_free(controller_t *ctl) {
	ctl->state_fn= NULL;
	main_notify_controller_freed(ctl);
}

/** Run all processing needed for the controller for this time slice
 * This function is mainly a wrapper that repeatedly executes the current state until
 * the state_fn returns false.  We then flush buffers and decide what to wake on.
 */
void ctl_run() {
	int i, j;
	controller_t *ctl;
	ctl_state_fn_t *prev_state;
	int64_t lateness, next_check_ts;
	
	// list is very small, so just iterate all, allocated or not.
	for (i= 0, ctl= client; i < CONTROLLER_MAX_CLIENTS; ctl= &client[++i]) {
		// non-null state means client is allocated
		if (!ctl->state_fn)
			continue;

		if (ctl->recv_fd >= 0) {
			// if waiting on input buffer, try reading more now
			if (FD_ISSET(ctl->recv_fd, &wake->fd_read) || FD_ISSET(ctl->recv_fd, &wake->fd_err)) {
				FD_CLR(ctl->recv_fd, &wake->fd_read);
				FD_CLR(ctl->recv_fd, &wake->fd_err);
				ctl_read_more(ctl);
			}
		}
		if (ctl->send_fd >= 0) {
			// if waiting on output buffer, try flushing now
			if (FD_ISSET(ctl->send_fd, &wake->fd_write) || FD_ISSET(ctl->send_fd, &wake->fd_err)) {
				FD_CLR(ctl->send_fd, &wake->fd_write);
				FD_CLR(ctl->send_fd, &wake->fd_err);
				ctl_flush_outbuf(ctl);
			}
		}
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
			if (!ctl_flush_outbuf(ctl) && ctl->send_fd >= 0) {
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

/** Report any new signals to the client
 *
 * We store the last signal event which we notified each client about, and now
 * we look for anything newer than that event.  sig_get_new_events() should be
 * doing appropriate synchronization to prevent race conditions.
 */
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
			log_debug("controller[%d] command length exceeds %d bytes, discarding", ctl->id, CONTROLLER_RECV_BUF_SIZE);
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
	log_debug("controller[%d] command: \"%s\"", ctl->id, ctl->recv_buf);
	ctl->state_fn= ctl_state_run_command;
	return true;
}

/** Dispatch the controller's current command
 *
 * The current command string was found in ctl_state_next_command, and now
 * we check whether it is a known command and dispatch it to a handler function.
 * Then, we check the result of the handler and if it fails we report an error.
 */
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
	else if (ctl->recv_overflow) {
		ctl_notify_error(ctl, "line too long");
		log_error("controller[%d] command exceeds buffer size");
	}
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
			if (!cmd) {
				ctl_notify_error(ctl, "Unknown command: %.*s", ctl->command_name.len, ctl->command_name.data);
				log_error("controller[%d] sent unknown command %.*s", ctl->command_name.len, ctl->command_name.data);
			}
			// dispatch it (returns false if it encounters an error, and sets ctl->command_error)
			else if (!cmd->fn(ctl)) {
				ctl_notify_error(ctl, "%s, for command \"%.*s%s\"", ctl->command_error, ctl->line_len > 30? 30 : ctl->line_len, ctl->recv_buf, ctl->line_len > 30? "...":"");
				log_error("controller[%d] command failed: '%.*s'%s", ctl->id, ctl->line_len > 90? 90 : ctl->line_len, ctl->recv_buf, ctl->line_len > 90? "...":"");
				log_error("  with error: '%.*s'", ctl->command_error);
			}
		}
	}
	return true;
}

/** Reset command-related variables, and advance read buffer.
 *
 * The command remains in the read buffer while it is being processed.
 * Now that we're done, we shift the read buffer to make room for more.
 */
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
must not contain ASCII control characters (other than TAB and NewLine).
Events are delivered in this same format.

In practice, ASCII control characters shouldn't be needed, and the
absence of quoting/escaping makes the protocol easier to implement
in your script.

=head2 COMMANDS

=over 4

=item echo ANY ARGUMENT LIST

Generates an event composed of the given arguments.  The main purpose of this
command is to synchronize communications with daemonproxy, by sending a unique
string and then waiting for the event containing that string, which means that
daemonproxy has completed every command sent before the echo as well.

You might find other clever uses for this command, since you can basically ask
daemonproxy to generate any event of your choice.

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
CLOSE_TIMEOUT seconds, daemonproxy will close the pipe, which hopefully sends
SIGPIPE to the controller and kills it, after which daemonproxy will restart
it if configured to do so.

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
=item chdir PATH

Tell daemonproxy to perform a chdir.  Use with extreme caution, because service
arguments might refer to executables or files by relative paths.

=cut
*/
bool ctl_cmd_chdir(controller_t *ctl) {
	strseg_t path;
	if (!ctl_get_arg(ctl, &path)) {
		ctl->command_error= "missing path argument";
		return false;
	}
	if (ctl_peek_arg(ctl, NULL)) {
		ctl->command_error= "unexpected argument after path";
		return false;
	}
	assert(path.data[path.len] == '\0');
	if (chdir(path.data) < 0) {
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"chdir failed: %s", strerror(errno));
		ctl->command_error= ctl->command_error_buf;
		return false;
	}
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
			svc_get_reap_ts(svc), svc_get_wstat(svc), svc_get_pid(svc));
 case 2:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 2; break; }
		ctl_notify_svc_tags(ctl, svc_get_name(svc), svc_get_tags(svc));
 case 3:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 3; break; }
		ctl_notify_svc_argv(ctl, svc_get_name(svc), svc_get_argv(svc));
 case 4:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 4; break; }
		ctl_notify_svc_fds(ctl, svc_get_name(svc), svc_get_fds(svc));
 case 5:
		if (!ctl_out_buf_ready(ctl)) { ctl->command_substate= 5; break; }
		ctl_notify_svc_auto_up(ctl, svc_get_name(svc), svc_get_restart_interval(svc), svc_get_triggers(svc));
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
=item fd.pipe NAME_READ NAME_WRITE FLAGS

Create a pipe, with the read-end named NAME_READ and write-end named
NAME_WRITE.  Re-using an existing name will close the old handle.

FLAGS can be a comma-delimited combination of: nonblock, unix, inet, inet6,
tcp, udp, dgram, seqpacket.  If any of the socket flags are used the pipe will
be a bi-directional socketpair().  Default socket domain is unix.  Default
socket type is stream.

=cut
*/
bool ctl_cmd_fd_pipe(controller_t *ctl) {
	fd_t *fd;
	int pair[2];
	strseg_t read_side, write_side, opt, opts;
	fd_flags_t flags;
	int sock_domain, sock_type, sock_proto;
	memset(&flags, 0, sizeof(flags));
	
	if (!ctl_get_arg_fd(ctl, false, true, &read_side, NULL)
		|| !ctl_get_arg_fd(ctl, false, true, &write_side, NULL))
		return false;
	
	// Check for optional flags
	if (ctl_get_arg(ctl, &opts)) {
		#define STRMATCH(flag) (opt.len == strlen(flag) && 0 == memcmp(opt.data, flag, opt.len))
		while (opts.len > 0) {
			opt= opts;
			strseg_split_1(&opt, ',', &opts);
			if (opt.len <= 0) continue;
			
			switch (opt.data[0]) {
			case '-':
				if (opt.len == 1) { continue; }
			case 'u':
				if (STRMATCH("unix"))  { flags.socket= true; flags.sock_inet= false; continue; }
				if (STRMATCH("udp"))   { flags.socket= true; flags.sock_inet= true; flags.sock_dgram= true; continue; }
				break;
			case 't':
				if (STRMATCH("tcp"))   { flags.socket= true; flags.sock_inet= true; flags.sock_dgram= false; continue; }
				break;
			case 'd':
				if (STRMATCH("dgram")) { flags.socket= true; flags.sock_dgram= true; continue; }
				break;
			case 'i':
				if (STRMATCH("inet"))  { flags.socket= true; flags.sock_inet= true; continue; }
			#ifdef AF_INET6
				if (STRMATCH("inet6")) { flags.socket= true; flags.sock_inet6= true; continue; }
			#endif
				break;
			case 's':
				if (STRMATCH("stream"))    { flags.socket= true; flags.sock_dgram= false; flags.sock_seq= false; continue; }
				if (STRMATCH("seqpacket")) { flags.socket= true; flags.sock_seq= true; continue; }
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
	}
	
	if (flags.socket) {
		sock_domain= flags.sock_inet? AF_INET
		#ifdef AF_INET6
			: flags.sock_inet6? AF_INET6
		#endif
			: AF_UNIX;
		sock_type=   flags.sock_dgram? SOCK_DGRAM : flags.sock_seq? SOCK_SEQPACKET : SOCK_STREAM;
		sock_proto= 0;
		if (0 != socketpair(sock_domain, sock_type, sock_proto, pair)) {
			snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
				"socketpair() failed: %s", strerror(errno));
			ctl->command_error= ctl->command_error_buf;
			return false;
		}
	}
	else {
		if (0 != pipe(pair)) {
			snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
				"pipe() failed: %s", strerror(errno));
			ctl->command_error= ctl->command_error_buf;
			return false;
		}
	}
	
	fd= fd_new_pipe(read_side, pair[0], write_side, pair[1], &flags);
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
		if (!opt.len) continue;
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
		ctl->command_error= "Unable to allocate new file descriptor object";
		return false;
	}

	ctl_notify_fd_state(NULL, fd);
	return true;
}

/*
=item fd.socket NAME FLAG1,FLAG2,.. ADDRSPEC

Creates a socket and optionally binds it to an address and optionally starts
a listen queue for it.  Re-using an existing name will close the old handle.
FLAGS may be unix,udp,tcp,inet,inet6,stream,dgram,seqpacket,nonblock,mkdir,
bind,listen.  listen implies bind.  Providing ADDRSPEC implies bind.  The
default socket type is unix,stream if FLAGS doesn't pick one.  mkdir only
takes effect if the socket type is unix.

There is no way to start a connection TO an address with daemonproxy, nor is
one planned, because connecting would entail a state machine and that
can be better handled within a service.

=cut
*/
bool ctl_cmd_fd_socket(controller_t *ctl) {
	int f;
	int sock_domain, sock_type, sock_proto;
	fd_flags_t flags;
	fd_t *fd;
	strseg_t fdname, opts, opt, optval, addrspec;

	if (!ctl_get_arg_fd(ctl, false, true, &fdname, NULL))
		return false;
	
	if (!ctl_get_arg(ctl, &opts)) {
		ctl->command_error= "missing flags argument";
		return false;
	}
	
	memset(&flags, 0, sizeof(flags));
	memset(&addrspec, 0, sizeof(addrspec));
	#define STRMATCH(flag) (opt.len == strlen(flag) && 0 == memcmp(opt.data, flag, opt.len))
	flags.socket= true;
	while (strseg_tok_next(&opts, ',', &opt)) {
		if (!opt.len) continue;
		// If option has an '=', break it into name=value
		optval= opt, strseg_tok_next(&optval, '=', &opt);
		switch (opt.data[0]) {
		case 'b':
			if (STRMATCH("bind"))  { flags.bind= true; continue; }
			break;
		case 'l':
			if (STRMATCH("listen")) {
				flags.bind= true;
				if (optval.len > 0) {
					int64_t val;
					if (!strseg_atoi(&optval, &val) || val >= (1<<16) || val <= 0) {
						ctl->command_error= "invalid listen queue length";
						return false;
					}
					flags.listen= (uint16_t) val;
				}
				else {
					flags.listen= 32;
				}
				continue;
			}
			break;
		case 'u':
			if (STRMATCH("unix"))  { flags.sock_inet= false; continue; }
			if (STRMATCH("udp"))   { flags.sock_inet= true; flags.sock_dgram= true; continue; }
			break;
		case 't':
			if (STRMATCH("tcp"))   { flags.sock_inet= true; flags.sock_dgram= false; continue; }
			break;
		case 'd':
			if (STRMATCH("dgram")) { flags.sock_dgram= true; continue; }
			break;
		case 'i':
			if (STRMATCH("inet"))  { flags.sock_inet= true; continue; }
		#ifdef AF_INET6
			if (STRMATCH("inet6")) { flags.socket= true; flags.sock_inet6= true; continue; }
		#endif
			break;
		case 's':
			if (STRMATCH("stream"))    { flags.sock_dgram= false; flags.sock_seq= false; continue; }
			if (STRMATCH("seqpacket")) { flags.sock_seq= true; continue; }
			break;
		case 'n':
			if (STRMATCH("nonblock")) { flags.nonblock= true; continue; }
			break;
		case 'm':
			if (STRMATCH("mkdir")) { flags.mkdir= true; continue; }
			break;
		}
		
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"unknown flag \"%.*s\"\n", opt.len, opt.data);
		ctl->command_error= ctl->command_error_buf;
		return false;
	}
	#undef STRMATCH
	
	sock_domain= flags.sock_inet? AF_INET
	#ifdef AF_INET6
		: flags.sock_inet6? AF_INET6
	#endif
		: AF_UNIX;
	sock_type=   flags.sock_dgram? SOCK_DGRAM : flags.sock_seq? SOCK_SEQPACKET : SOCK_STREAM;
	sock_proto= 0;

	struct sockaddr_storage addr;
	int addrlen= sizeof(addr);
	
	if (ctl_get_arg(ctl, &addrspec)) {
		flags.bind= true;
		if (!strseg_parse_sockaddr(&addrspec, sock_domain, &addr, &addrlen)) {
			ctl->command_error= "invalid address";
			return false;
		}
	}
	else if (flags.bind) {
		ctl->command_error= "expected address argument";
		return false;
	}

	if (ctl_peek_arg(ctl, NULL)) {
		ctl->command_error= "unexpected argument after address";
		return false;
	}

	if (flags.mkdir && sock_domain == AF_UNIX)
		// we don't check success on this.  we just let open() fail and check that.
		create_missing_dirs(((struct sockaddr_un*)&addr)->sun_path);

	const char *failed= ((f= socket(sock_domain, sock_type, sock_proto)) < 0)? "socket"
		: (flags.bind && bind(f, (struct sockaddr*) &addr, addrlen) < 0)? "bind"
		: (flags.listen && listen(f, flags.listen) < 0)? "listen"
		: (flags.nonblock && fcntl(f, F_SETFL, O_NONBLOCK) < 0)? "fcntl(O_NONBLOCK)"
		: NULL;
	
	if (failed) {
		snprintf(ctl->command_error_buf, sizeof(ctl->command_error_buf),
			"%s: %s", failed, strerror(errno));
		ctl->command_error= ctl->command_error_buf;
		
		if (f >= 0) close(f);
		return false;
	}

	fd= fd_new_file(fdname, f, flags, addrspec);
	if (!fd) {
		close(f);
		ctl->command_error= "Unable to allocate new file descriptor object";
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
bool ctl_cmd_fd_delete(controller_t *ctl) {
	fd_t *fd;

	if (!ctl_get_arg_fd(ctl, true, true, NULL, &fd))
		return false;

	ctl_write(NULL, "fd.state	%s	deleted\n", fd_get_name(fd));
	fd_delete(fd);
	return true;
}

/*
=item service.tags NAME TAG_1 TAG_2 ... TAG_N

Set some ad-hoc metadata for this service, for use by the controller script.
Beware! if you use a fixed-size service memory pool you won't be able to
fit much data here.  Also everything you store here will be held in memory,
bulking up daemonproxy's footprint.  This is mainly intended for small tags
and IDs which might be inconvenient to store within the service name.

If you have lots of metadata for a service, consider storing it in a file
or database, keyed by the service name.
=cut
*/
bool ctl_cmd_svc_tags(controller_t *ctl) {
	service_t *svc;
	
	if (!ctl_get_arg_service(ctl, false, NULL, &svc))
		return false;
	
	if (!svc_set_tags(svc, ctl->command.len >= 0? ctl->command : STRSEG(""))) {
		ctl->command_error= "unable to set tags";
		return false;
	}
	
	ctl_notify_svc_tags(NULL, svc_get_name(svc), svc_get_tags(svc));
	return true;
}

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
=item service.auto_up NAME MIN_INTERVAL [TRIGGER]...

Start the service (no more rapidly than MIN_INTERVAL seconds apart) if any of
the triggers are true.

MIN_INTERVAL counts from the time of the previous start attempt, so if the
service has been running longer than MIN_INTERVAL and it exits while a trigger
is true, it will be restarted immediately.  MIN_INTERVAL cannot be less than 1
second.  A MIN_INTERVAL of '-' disables auto-up.

Currently, triggers are 'always', SIGINT, SIGHUP, SIGTERM, SIGUSR1, SIGUSR2,
SIGQUIT.

'always' means the service will always start if it is not already running.
Using 'always' with a large MIN_INTERVAL can give you a cron-like effect, if
you want a periodicaly-run service and don't care what specific time it runs.

Signal triggers cause the service to start if the pending count of that signal
is nonzero.  (and the service is expected to issue the command "signal.clear"
to reset the count to zero, to prevent being started again)

=cut
*/
bool ctl_cmd_svc_auto_up(controller_t *ctl) {
	service_t *svc;
	int64_t interval;
	strseg_t tmp;

	if (!ctl_get_arg_service(ctl, false, NULL, &svc))
		return false;

	tmp= ctl->command;
	if (!ctl_get_arg_int(ctl, &interval)) {
		// '-' means re-use old value
		if (tmp.len >= 1 && tmp.data[0] == '-' && (tmp.len == 1 || tmp.data[1] == '\t')) {}
		else
			return false;
	}
	else if (interval < 1 || (interval >> 31)) {
		ctl->command_error= "invalid interval";
		return false;
	}
	else {
		interval <<= 32;
		svc_set_restart_interval(svc, interval);
	}

	if (!svc_set_triggers(svc, ctl->command.len > 0? ctl->command : STRSEG(""))) {
		ctl->command_error= "unable to set auto_up triggers";
		return false;
	}
	
	ctl_notify_svc_auto_up(NULL, svc_get_name(svc), svc_get_restart_interval(svc), svc_get_triggers(svc));
	return true;
}

/*
=item service.start NAME [FUTURE_TIMESTAMP]

Start the service, optionally at a future time (or cancel a previous request).

FUTURE_TIMESTAMP is an integer of seconds according to CLOCK_MONOTONIC.
(This might be extended to allow fractional seconds in the future.)
The service.start will take place as soon as that time is past, but if
the timestamp is more than 100000 seconds in the past it is considered
an error.  The service.state will become 'start'.

If FUTURE_TIMESTAMP is the string "-", a pending start event will be
canceled.  However, because commands are asynchronous the service might
get started anyway.  Watch for the service.state event to find out whether
it started or was canceled.

Starting a service can reveal configuration errors, such as invalid FD names,
or an argv that can't be executed.  Error messages are events, so they are
asynchronous and might be received in any order.

=cut
*/
bool ctl_cmd_svc_start(controller_t *ctl) {
	const char *argv;
	int64_t starttime_ts;
	service_t *svc;
	strseg_t when;
	
	if (!ctl_get_arg_service(ctl, true, NULL, &svc))
		return false;
	
	// Optional timestamp for service start
	if (ctl_peek_arg(ctl, &when)) {
		if (0 == strseg_cmp(when, STRSEG("-"))) {
			if (svc_cancel_start(svc))
				return true;
			ctl->command_error= "no service.start pending";
			return false;
		}
		if (!ctl_get_arg_ts(ctl, &starttime_ts) || starttime_ts - wake->now < -100000) {
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
	
	if (svc_handle_start(svc, starttime_ts))
		return true;
	ctl->command_error= "service is not startable";
	return false;
}

/*
=item service.signal NAME SIGNAL [FLAGS]

Send SIGNAL to the named service's pid, if it is running.  If you specify the
flag "group" and the service is leading a process group, then the entire group
receives the signal.

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
=item service.delete NAME

Delete a service.  The service can only be deleted if it is not currently
running.  Once deleted, there is no trace of the service's state.

=cut
*/
bool ctl_cmd_svc_delete(controller_t *ctl) {
	service_t *svc;

	if (!ctl_get_arg_service(ctl, true, NULL, &svc))
		return false;
	if (svc_get_pid(svc) > 0) {
		ctl->command_error= "service is running";
		return false;
	}

	ctl_write(NULL, "service.state	%s	deleted	-	-	-	-	-	-\n", svc_get_name(svc));
	svc_delete(svc);
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
=item log.dest fd FD_NAME

Redirect daaemonproxy's logging to named file descriptor.  FD_NAME must be a
valid name, but it does not need to exist yet.  The logging system will check
this name until it is available, and then resume logging.  Likewise if the
descriptor by that name is deleted or re-used.

WARNING: the file descriptor will be put into non-blocking mode, so it is best
not to share this descriptor with other processes, especially processes that
might reset it to a blocking state and cause daemonproxy to hang on a blocked
logging pipe. (However, if you're one of those types who preferrs your daemons
freeze up when the logging is interrupted, then here's your workaround.)

=cut
*/
bool ctl_cmd_log_dest(controller_t *ctl) {
	strseg_t arg, fd_name;
	fd_t *fd;

	if (!ctl_get_arg(ctl, &arg))
		return false;

	if (strseg_cmp(arg, STRSEG("fd")) == 0) {
		if (!ctl_get_arg_fd(ctl, false, false, &fd_name, &fd))
			return false;

		if (!fd)
			ctl_write(ctl, "warning	fd \"%.*s\" does not exist\n", fd_name.len, fd_name.data);

		log_fd_set_name(fd_name);
		return true;
	}
	else {
		ctl->command_error= "Unknown logging type";
		return false;
	}
}

/*
=item signal.clear SIGNAL COUNT

Decrements the count of one signal.  Daemonproxy increments the count each time
a signal is received, and generates an event for any listening controllers.
Statedumps will continue to show the signal while it has a nonzero count.
This command decrements the count, allowing a controller to know that the signal
has been dealt with.

=cut
*/
bool ctl_cmd_signal_clear(controller_t *ctl) {
	int sig;
	int64_t count;

	if (!ctl_get_arg_signal(ctl, &sig))
		return false;
	if (!ctl_get_arg_int(ctl, &count))
		return false;

	sig_mark_seen(sig, count);
	return true;
}

/*
=item socket.create OPTIONS PATH

Create the controller socket at the designated path.  Options must be empty
or the literal string "-". (options will be added in the future)

Only one controller socket may exist.  If create is called a second time, the
previous socket will be unlinked.

=cut
*/
bool ctl_cmd_socket_create(controller_t *ctl) {
	strseg_t opts, path;

	if (!ctl_get_arg(ctl, &opts))
		return false;

	if (!ctl_get_arg(ctl, &path))
		return false;

	if (opts.len > 1 || (opts.len != 0 && opts.data[0] != '-')) {
		ctl->command_error= "Invalid options";
		return false;
	}
	
	if (!control_socket_start(path)) {
		ctl->command_error= "Failed to create control socket";
		return false;
	}
	return true;
}

/*
=item socket.delete

Takes no arguments.  Cleans up the previously created socket.  If no socket
exists, this is a no-op.

=cut
*/
bool ctl_cmd_socket_delete(controller_t *ctl) {
	control_socket_stop();
	return true;
}

/*
=item terminate EXIT_CODE [GUARD_CODE]

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
	if (opt_terminate_guard) {
		if (!ctl_get_arg_int(ctl, &x)) {
			ctl->command_error= "terminate guard code required";
			return false;
		}
		if (opt_terminate_guard != x) {
			ctl->command_error= "incorrect terminate guard code";
			return false;
		}
	}
	
	// Can't exit if running as pid 1
	if (opt_terminate_guard && !opt_exec_on_exit) {
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
		if (opt_terminate_guard) {
			ctl->command_error= "terminate guard code is already set";
			return false;
		}
		opt_terminate_guard= code;
		return true;
	}
	else {
		if (!opt_terminate_guard) {
			ctl->command_error= "terminate guard was not set";
			return false;
		}
		if (code != opt_terminate_guard) {
			ctl->command_error= "incorrect guard code";
			return false;
		}
		opt_terminate_guard= 0;
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
	return ctl_write(ctl, "signal	SIG%s	%d	%d\n", signame? signame : "-?", (int)(sig_ts>>32), count);
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
=item service.tags NAME TAG_1 TAG_2 ... TAG_N

Tags for the service have changed.

=cut
*/
bool ctl_notify_svc_tags(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.tags	%s	%s\n", name, tsv_fields);
}

/*
=item service.args NAME ARG_1 ARG_2 ... ARG_N

Arguments for the service have changed.

=cut
*/
bool ctl_notify_svc_argv(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.args	%s	%s\n", name, tsv_fields);
}

/*
=item service.fds NAME HANDLE_1 HANDLE_2 ... HANDLE_N

File handles for the service have changed.

=cut
*/
bool ctl_notify_svc_fds(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.fds	%s	%s\n", name, tsv_fields);
}

/*
=item service.triggers NAME [TRIGGER_1], [TRIGER_2] ...

Triggers for auto-starting the service have changed.

=cut
*/
bool ctl_notify_svc_auto_up(controller_t *ctl, const char *name, int64_t interval, const char *triggers_tsv) {
	if (triggers_tsv && triggers_tsv[0])
		ctl_write(ctl, "service.auto_up	%s	%d	%s\n", name, (int)(interval>>32), triggers_tsv);
	else
		ctl_write(ctl, "service.auto_up	%s	-\n", name);
	return true;
}

/*
=item fd.state NAME TYPE FLAGS DESCRIPTION

TYPE is 'file', 'pipe', 'socket', 'special', or 'deleted'.  Deleted means the file
handle has just been removed and no longer exists.  Type 'file' has FLAGS
that match the flags used to open it (though possibly in a different order).
Type 'pipe' refers to both pipes and socketpairs.  Flags for a pipe are 'to' or 'from',
but if it is a socketpair it also has flags for the domain and type.
DESCRIPTION is the filename (possibly truncated), the pipe-peer handle name,
the bound socket address, or a free-form string describing the handle.

=cut
*/
bool ctl_notify_fd_state(controller_t *ctl, fd_t *fd) {
	fd_t *peer;
	const char *name= fd_get_name(fd);
	fd_flags_t flags= fd_get_flags(fd);
	
	if (flags.pipe) {
		peer= fd_get_pipe_peer(fd);
		return ctl_write(ctl, "fd.state" "\t" "%s" "\t" "pipe" "\t" "%s%s%s%s" "\t" "%s\n",
			name,
			!flags.socket? "" : flags.sock_inet? "inet," : flags.sock_inet6? "inet6," : "unix,",
			!flags.socket? "" : flags.sock_dgram? "dgram," : flags.sock_seq? "seqpacket," : "stream,",
			flags.nonblock? "nonblock," : "",
			(flags.write || flags.socket)? "to":"from",
			peer? fd_get_name(peer) : "?"
		);
	}
	else if (flags.socket) {
		char listenbuf[32];
		if (flags.listen)
			snprintf(listenbuf, sizeof(listenbuf), ",listen=%d", flags.listen);
		return ctl_write(ctl, "fd.state" "\t" "%s" "\t" "%s" "\t" "%s%s%s%s%s%s" "\t" "%s\n",
			name,
			flags.special? "special" : "socket",
			flags.sock_inet? "inet" : flags.sock_inet6? "inet6" : "unix",
			flags.sock_dgram? ",dgram" : flags.sock_seq? ",seqpacket" : ",stream",
			flags.bind? ",bind" : "",
			flags.listen? listenbuf : "",
			flags.mkdir? ",mkdir" : "",
			flags.nonblock? ",nonblock" : "",
			fd_get_file_path(fd)
		);
		return true;
	}
	else {
		return ctl_write(ctl, "fd.state" "\t" "%s" "\t" "%s" "\t" "%s%s%s%s%s%s" "\t" "%s\n",
			name, flags.special? "special" : "file",
			(flags.write? (flags.read? "read,write":"write"):"read"),
			(flags.append? ",append":""), (flags.create? ",create":""),
			(flags.trunc? ",trunc":""), (flags.nonblock? ",nonblock":""),
			(flags.mkdir? ",mkdir":""), fd_get_file_path(fd));
	}
}

/*----------------------------------------------------------------------------
 * End of events

=item error MESSAGE

Error events are reported free-form, with "error" as the first tab delimited
token, but MESSAGE being arbitrary ascii text.

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
		log_trace("controller[%d] input read failed: %d %s", ctl->id, n, strerror(e));
		if (n == 0 || (e != EINTR && e != EAGAIN && e != EWOULDBLOCK)) {
			if (n < 0)
				log_error("read(client[%d])): %s", ctl->id, strerror(e));
			// EOF.  Close file descriptor
			close(ctl->recv_fd);
			ctl->recv_fd= -1;
		}
		errno= e;
		return false;
	}
	ctl->recv_buf_pos += n;
	log_trace("controller[%d] read %d bytes (%d in recv buf)", ctl->id, n, ctl->recv_buf_pos);
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
			
			log_debug("client[%d]: can't write msg, %d > buffer free %d", dest[i]->id, msg_len, buf_free);
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
			log_debug("client[%d] event: \"%.*s\"", dest[i]->id, msg_len, msg_data);
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
	int n, eol;
	while (ctl->send_buf_pos) {
		// find end of last line in buffer
		for (eol= ctl->send_buf_pos-1; eol >= 0; eol--)
			if (ctl->send_buf[eol] == '\n')
				break;
		log_trace("controller[%d] write buffer %d bytes pending, final eol at %d, %s",
			ctl->id, ctl->send_buf_pos, eol, ctl->send_overflow? "(overflow flag set)" : "");
		// if no send_fd, discard buffer
		if (ctl->send_fd == -1)
			ctl->send_buf_pos= 0;
		// if no eol, can't continue
		// This prevents partial lines from being written, which could get
		// interrupted by an overflow condition and result in the controller
		// script seeing a half-event.
		else if (eol < 0) {
			// if overflow is set, discard partial line.
			if (ctl->send_overflow) ctl->send_buf_pos= 0;
			else return false;
		}
		// else write as much as we can (on nonblocking fd)
		else {
			n= write(ctl->send_fd, ctl->send_buf, eol+1);
			if (n > 0) {
				log_trace("controller[%d] flushed %d bytes", ctl->id, n);
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
				log_debug("controller[%d] outbuf write failed: %s", ctl->id, strerror(errno));
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

/** Extract the next argument as a timestamp
 */
bool ctl_get_arg_ts(controller_t *ctl, int64_t *ts) {
	strseg_t str;
	if (!strseg_tok_next(&ctl->command, '\t', &str)
		|| !strseg_atoi(&str, ts)
		|| str.len > 0   // should consume all of str
	) {
		ctl->command_error= "Expected integer";
		return false;
	}
	*ts <<= 32; // create fixed-point 32.32 timestamp
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
		ctl->command_error= existing? "No such service" : "Unable to allocate new service";
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

