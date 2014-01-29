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
	int64_t send_abort_ts;
	
	int line_len;             // length of current TSV line in recv_buf
	strseg_t command_name;    // strseg of command name (within recv_buf)
	strseg_t command_arg_str; // TSV string for command to process (within recv_buf)
	int command_substate;     // set to 0 at start of each command
	
	char statedump_current[NAME_LIMIT];
	int  statedump_part;
};

controller_t client[CONTROLLER_MAX_CLIENTS];

// Each of the following functions returns true/false of whether to continue
//  processing (true), or yield until later (false).

#define STATE(name, ...) static bool name(controller_t *ctl)
STATE(ctl_state_close);
STATE(ctl_state_read_command);
STATE(ctl_state_cmd_overflow);
STATE(ctl_state_cmd_unknown);
STATE(ctl_state_cmd_statedump,         "statedump");
STATE(ctl_state_cmd_svc_args_set,      "service.args");
STATE(ctl_state_cmd_svc_meta,          "service.meta");
STATE(ctl_state_cmd_svc_meta_set,      "service.meta.set");
STATE(ctl_state_cmd_svc_meta_apply,    "service.meta.apply");
STATE(ctl_state_cmd_svc_fds_set,       "service.fds");
STATE(ctl_state_cmd_svc_start,         "service.start");
STATE(ctl_state_cmd_fd_pipe,           "fd.pipe");
STATE(ctl_state_cmd_fd_open,           "fd.open");
STATE(ctl_state_cmd_exit,              "exit");
STATE(ctl_state_cmd_terminate,         "terminate");

static bool ctl_ctor(controller_t *ctl, int recv_fd, int send_fd);
static void ctl_dtor(controller_t *ctl);
static bool ctl_read_more(controller_t *ctl);
static bool ctl_flush_outbuf(controller_t *ctl);

typedef struct ctl_command_table_entry_s {
	const char *command;
	ctl_state_fn_t *state_fn;
} ctl_command_table_entry_t;

#include "controller_data.autogen.c"

const ctl_command_table_entry_t * ctl_find_command(const char *buffer) {
	const ctl_command_table_entry_t *result= &ctl_command_table[ ctl_command_hash_func(buffer) ];
	return result->state_fn && strstr(buffer, result->command) == buffer? result : NULL;
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
	int i;
	for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++)
		if (!client[i].state_fn)
			return ctl_ctor(&client[i], recv_fd, send_fd)? &client[i] : NULL;
	return NULL;
}

static bool ctl_ctor(controller_t *ctl, int recv_fd, int send_fd) {
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
	// initialize object.  non-null state marks it as allocated
	memset(ctl, 0, sizeof(controller_t));
	ctl->state_fn= &ctl_state_read_command;
	ctl->recv_fd= recv_fd;
	ctl->send_fd= send_fd;
	return true;
}

static void ctl_dtor(controller_t *ctl) {
	log_debug("destroying client %d", ctl - client);
	if (ctl->recv_fd >= 0) close(ctl->recv_fd);
	if (ctl->send_fd >= 0) close(ctl->send_fd);
	ctl->state_fn= NULL;
}

bool ctl_state_close(controller_t *ctl) {
	if (ctl->send_fd >= 0)
		if (!ctl_flush_outbuf(ctl))
			return false; // remain in this state and try again later
	ctl_dtor(ctl);
	return false; // don't run any more state iterations, because there aren't any
}

#define END_CMD(cond) end_cmd_(ctl, cond)
static inline bool end_cmd_(controller_t *ctl, bool final_cond) {
	if (final_cond) {
		ctl->state_fn= ctl_state_read_command;
		return true;
	}
	return false;
}

// This state is really just a fancy non-blocking readline(), with special
// handling for long lines (and EOF, when reading the config file)
bool ctl_state_read_command(controller_t *ctl) {
	int prev;
	char *eol, *p, *p2;
	const ctl_command_table_entry_t *cmd;

	// clean up old command, if any.
	if (ctl->line_len > 0) {
		ctl->recv_buf_pos -= ctl->line_len;
		memmove(ctl->recv_buf, ctl->recv_buf + ctl->line_len, ctl->recv_buf_pos);
		ctl->line_len= 0;
	}
	
	// see if we have a full line in the input.  else read some more.
	eol= (char*) memchr(ctl->recv_buf, '\n', ctl->recv_buf_pos);
	while (!eol && ctl->recv_fd >= 0) {
		// if buffer is full, then command is too big, and we ignore the rest of the line
		if (ctl->recv_buf_pos >= CONTROLLER_RECV_BUF_SIZE) {
			// In case its a comment, preserve comment character (long comments are not an error)
			ctl->recv_overflow= true;
			ctl->recv_buf_pos= 1;
		}
		// Try reading more from our non-blocking handle
		prev= ctl->recv_buf_pos;
		if (ctl_read_more(ctl)) {
			log_trace("controller read %d bytes", ctl->recv_buf_pos - prev);
			// See if new data has a "\n"
			eol= (char*) memchr(ctl->recv_buf + prev, '\n', ctl->recv_buf_pos - prev);
		}
		else {
			log_trace("controller unable to read more yet");
			// done for now, until more data available on pipe
			return false;
		}
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
			log_warn("Line ends with EOF... processing anyway");
			ctl->recv_buf[ctl->recv_buf_pos]= '\n';
			eol= ctl->recv_buf + ctl->recv_buf_pos++;
		}
		// if no eol, then ignore final partial command
		if (!eol) {
			log_warn("Line ends with EOF... ignored");
			ctl->recv_buf_pos= 0;
			ctl->state_fn= ctl_state_close;
			return true;
		}
	}

	// We now have a complete line
	ctl->line_len= eol - ctl->recv_buf + 1;
	*eol= '\0';
	log_debug("controller got line: \"%s\"", ctl->recv_buf);
	// Ignore overflow lines, empty lines, lines starting with #, and lines that start with whitespace
	if (ctl->recv_buf[0] == '\n'
		|| ctl->recv_buf[0] == '\r'
		|| ctl->recv_buf[0] == '#'
		|| ctl->recv_buf[0] == ' '
		|| ctl->recv_buf[0] == '\t'
	)
	{
		log_trace("Ignoring comment line");
		// continue back to this state by doing nothing
	}
	// check for command overflow
	else if (ctl->recv_overflow)
		ctl->state_fn= ctl_state_cmd_overflow;
	// else try to parse and dispatch it
	else {
		// break off the first N tsv fields into an argv[] array
		p= p2= ctl->recv_buf;
		while (*p2 != '\t' && p2 < eol) p2++;
		*p2= '\0';
		ctl->command_name= (strseg_t){ p, p2-p };
		if (p2 < eol) p2++;
		ctl->command_arg_str= (strseg_t){ p2, eol - p2 };
		// Now see if we can find the command
		if ((cmd= ctl_find_command(p)))
			ctl->state_fn= cmd->state_fn;
		else
			ctl->state_fn= ctl_state_cmd_unknown;
		ctl->command_substate= 0;
	}
	return true;
}

bool ctl_state_cmd_overflow(controller_t *ctl) {
	if (!ctl_write(ctl, "overflow\n"))
		return false;
	ctl->state_fn= ctl_state_read_command;
	return true;
}

bool ctl_state_cmd_unknown(controller_t *ctl) {
	// find command string
	return END_CMD( ctl_notify_error(ctl, "Unknown command: %.*s",
		ctl->command_name.len, ctl->command_name.data) );
}

bool ctl_state_cmd_exit(controller_t *ctl) {
	// they asked for it...
	if (ctl->recv_fd >= 0) close(ctl->recv_fd);
	ctl->recv_fd= -1;
	ctl->state_fn= ctl_state_close;
	return false;
}

bool ctl_state_cmd_terminate(controller_t *ctl) {
	main_terminate= true;
	wake->next= wake->now;
	return END_CMD(true);
}

/** Statedump command, part 1: Initialize vars and pass to part 2.
 */
bool ctl_state_cmd_statedump(controller_t *ctl) {
	fd_t *fd= NULL;
	service_t *svc= NULL;
	if (ctl->command_substate > 2) {
		// resume the while loop where we left off, if service still exists
		svc= svc_by_name((strseg_t){ ctl->statedump_current, strlen(ctl->statedump_current) }, false);
		if (!svc) ctl->command_substate= 2;
	}
	
	switch (ctl->command_substate) {
	case 0:
		ctl->statedump_current[0]= '\0';
		ctl->command_substate= 1;
	case 1:
		/* Statedump command, part 1: iterate fd objects and dump each one.
		 * Uses an iterator of fd_t's name so that if fds are creeated or destroyed during our
		 * iteration, we can safely resume.  (and this iteration could be idle for a while if
		 * the controller script doesn't read its pipe)
		 */
		while ((fd= fd_iter_next(fd, (strseg_t){ ctl->statedump_current, strlen(ctl->statedump_current) }))) {
			if (!ctl_notify_fd_state(ctl, fd)) {
				// if state dump couldn't complete (output buffer full)
				//  save our position to resume later
				strcpy(ctl->statedump_current, fd_get_name(fd)); // length of name has already been checked
				return false;
			}
		}
		ctl->statedump_current[0]= '\0';
		ctl->command_substate= 2;
	case 2:
		/* Statedump command, part 2: iterate services and dump each one.
		 * Like part 2 above, except a service has 4 lines of output, and we need to be able to resume
		 * if a full buffer interrupts us half way through one service.
		 */
		while ((svc= svc_iter_next(svc, (strseg_t){ ctl->statedump_current, strlen(ctl->statedump_current) }))) {
			log_debug("service iter = %s", svc_get_name(svc));
			svc_check(svc);
			strcpy(ctl->statedump_current, svc_get_name(svc)); // length of name has already been checked
	case 3:
			if (!ctl_notify_svc_state(ctl, svc_get_name(svc), svc_get_up_ts(svc),
					svc_get_reap_ts(svc), svc_get_pid(svc), svc_get_wstat(svc))
			) {
				ctl->command_substate= 3;
				return false;
			}
	case 4:
			if (!ctl_notify_svc_meta(ctl, svc_get_name(svc), svc_get_meta(svc))) {
				ctl->command_substate= 4;
				return false;
			}
	case 5:
			if (!ctl_notify_svc_argv(ctl, svc_get_name(svc), svc_get_argv(svc))) {
				ctl->command_substate= 5;
				return false;
			}
	case 6:
			if (!ctl_notify_svc_fds(ctl, svc_get_name(svc), svc_get_fds(svc))) {
				ctl->command_substate= 6;
				return false;
			}
		}
	}
	return END_CMD(true);
}

bool ctl_extract_svc_arg(controller_t *ctl, strseg_t *line, service_t **svc, bool create, bool *notified) {
	strseg_t name;
	if (!strseg_tok_next(line, '\t', &name)) {
		*notified= ctl_notify_error(ctl, "Missing service name");
		return false;
	}
	if (!svc_check_name(name)) {
		*notified= ctl_notify_error(ctl, "Invalid service name: \"%.*s\"", name.len, name.data);
		return false;
	}
	if (!( *svc= svc_by_name(name, create) )) {
		*notified= create? ctl_notify_error(ctl, "Unable to create new service")
				: ctl_notify_error(ctl, "No such service");
		return false;
	}
	return true;
}

bool ctl_extract_fdname_arg(controller_t *ctl, strseg_t *line, strseg_t *name, bool existing, bool assignable, bool *notified) {
	fd_t *fd;
	if (!strseg_tok_next(line, '\t', name)) {
		*notified= ctl_notify_error(ctl, "Missing file descriptor name");
		return false;
	}
	if (!fd_check_name(*name)) {
		*notified= ctl_notify_error(ctl, "Invalid file descriptor name: \"%.*s\"", name->len, name->data);
		return false;
	}
	fd= fd_by_name(*name);
	if (assignable && fd && fd_get_flags(fd).is_const) {
		*notified= ctl_notify_error(ctl, "File descriptor \"%s\" cannot be altered", fd_get_name(fd));
		return false;
	}
	if (existing && !fd) {
		*notified= ctl_notify_error(ctl, "No such file descriptor: \"%s\"", fd_get_name(fd));
		return false;
	}
	return true;
}

/** service.args command
 * Set the argv for a service object
 * Also report the state change when done.
 */
bool ctl_state_cmd_svc_args_set(controller_t *ctl) {
	service_t *svc;
	bool notified;
	strseg_t line= ctl->command_arg_str;
	
	if (!ctl_extract_svc_arg(ctl, &line, &svc, true, &notified))
		return END_CMD(notified);
	
	if (line.len < 0)
		return END_CMD( ctl_notify_error(ctl, "Missing argument list") );
	
	if (!svc_set_argv(svc, line))
		return END_CMD( ctl_notify_error(ctl, "unable to set argv for service \"%s\"", svc_get_name(svc)) );
	ctl_notify_svc_argv(NULL, svc_get_name(svc), svc_get_argv(svc));
	return END_CMD(true);
}

bool ctl_state_cmd_svc_meta(controller_t *ctl) {
	service_t *svc;
	bool notified;
	strseg_t line= ctl->command_arg_str;
	
	if (!ctl_extract_svc_arg(ctl, &line, &svc, false, &notified))
		return END_CMD(notified);
	
	return END_CMD( ctl_notify_svc_meta(ctl, svc_get_name(svc), svc_get_meta(svc)) );
}

bool ctl_state_cmd_svc_meta_set(controller_t *ctl) {
	service_t *svc;
	bool notified;
	strseg_t line= ctl->command_arg_str;
	
	if (!ctl_extract_svc_arg(ctl, &line, &svc, true, &notified))
		return END_CMD(notified);
	
	if (line.len < 0)
		return END_CMD( ctl_notify_error(ctl, "Missing argument list") );

	if (!svc_set_meta(svc, line))
		return END_CMD( ctl_notify_error(ctl, "Unable to set meta for service \"%s\"", svc_get_name(svc)) );

	ctl_notify_svc_meta(NULL, svc_get_name(svc), svc_get_meta(svc));
	return END_CMD(true);
}

bool ctl_state_cmd_svc_meta_apply(controller_t *ctl) {
	service_t *svc;
	bool notified;
	strseg_t item, key, line= ctl->command_arg_str;

	if (!ctl_extract_svc_arg(ctl, &line, &svc, true, &notified))
		return END_CMD(notified);

	while (strseg_tok_next(&line, '\t', &item)) {
		if (!strseg_tok_next(&item, '=', &key))
			return END_CMD( ctl_notify_error(ctl, "Invalid metadata token \"%.*s\" (missing '=')", item.len, item.data) );
		if (!svc_apply_meta(svc, key, item))
			return END_CMD( ctl_notify_error(ctl, "Out of space for metadata change to service \"%s\"", svc_get_name(svc)) );
	}
	ctl_notify_svc_meta(NULL, svc_get_name(svc), svc_get_meta(svc));
	return END_CMD(true);
}

bool ctl_state_cmd_svc_fds_set(controller_t *ctl) {
	service_t *svc;
	bool notified;
	strseg_t line= ctl->command_arg_str;
	
	if (!ctl_extract_svc_arg(ctl, &line, &svc, true, &notified))
		return END_CMD(notified);
	
	if (line.len < 0)
		return END_CMD( ctl_notify_error(ctl, "Missing argument list") );

	if (!svc_set_fds(svc, line))
		return END_CMD( ctl_notify_error(ctl, "Unable to set file descriptors for service \"%s\"", svc_get_name(svc)) );
	ctl_notify_svc_meta(NULL, svc_get_name(svc), svc_get_fds(svc));
	return END_CMD(true);
}

/** service.exec command
 * Forks and execs named service, if it is not running.
 * Errors in specification (or if service is up) are reported immediately.
 * Results of exec attempt are reported via other events.
 */
bool ctl_state_cmd_svc_start(controller_t *ctl) {
	const char *argv;
	char *endp;
	int64_t starttime_ts;
	service_t *svc;
	bool notified;
	strseg_t starttime, line= ctl->command_arg_str;
	
	if (!ctl_extract_svc_arg(ctl, &line, &svc, false, &notified))
		return END_CMD(notified);
	
	// Optional timestamp for service start
	if (strseg_tok_next(&line, '\t', &starttime)) {
		starttime_ts= strtol( starttime.data, &endp, 10);
		if (endp - starttime.data != starttime.len)
			return END_CMD( ctl_notify_error(ctl, "Invalid timestamp") );
	}
	else starttime_ts= wake->now;
	
	argv= svc_get_argv(svc);
	if (!argv[0] || argv[0] == '\t' || argv[0] == '\0')
		return END_CMD( ctl_notify_error(ctl, "Missing/invalid argument list for \"%s\"", svc_get_name(svc)) );
	
	svc_handle_start(svc, starttime_ts);
	return END_CMD(true);
}

bool ctl_state_cmd_fd_pipe(controller_t *ctl) {
	fd_t *fd;
	bool notified;
	int pair[2];
	strseg_t read_side, write_side, line= ctl->command_arg_str;
	
	if (!ctl_extract_fdname_arg(ctl, &line, &write_side, false, true, &notified))
		return END_CMD(notified);
	
	if (!ctl_extract_fdname_arg(ctl, &line, &read_side, false, true, &notified))
		return END_CMD(notified);
	
	if (0 != pipe(pair))
		return END_CMD( ctl_notify_error(ctl, "pipe() failed: %s", strerror(errno)) );
	
	fd= fd_new_pipe(read_side, pair[0], write_side, pair[1]);
	if (!fd) {
		close(pair[0]);
		close(pair[1]);
		return END_CMD( ctl_notify_error(ctl, "Failed to create pipe") );
	}
	
	ctl_notify_fd_state(NULL, fd);
	ctl_notify_fd_state(NULL, fd_get_pipe_peer(fd));
	return END_CMD(true);
}

bool ctl_state_cmd_fd_open(controller_t *ctl) {
	bool notified;
	int f, open_flags;
	fd_flags_t flags;
	fd_t *fd;
	strseg_t fdname, opts, opt, path, line= ctl->command_arg_str;

	if (!ctl_extract_fdname_arg(ctl, &line, &fdname, false, true, &notified))
		return END_CMD(notified);
	
	if (!strseg_tok_next(&line, '\t', &opts))
		return END_CMD( ctl_notify_error(ctl, "Missing flags argument") );
	
	if (!strseg_tok_next(&line, '\t', &path))
		return END_CMD( ctl_notify_error(ctl, "Missing path argument") );
	
	if (line.len >= 0)
		return END_CMD( ctl_notify_error(ctl, "Unexpected argument after path") );
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
		ctl_notify_error(ctl, "Unknown flag \"%.*s\"", opt.len, opt.data);
	}
	#undef STRMATCH
	
	if (flags.mkdir)
		create_missing_dirs((char*)path.data);

	open_flags= (flags.write? (flags.read? O_RDWR : O_WRONLY) : O_RDONLY)
		| (flags.append? O_APPEND : 0) | (flags.create? O_CREAT : 0)
		| (flags.trunc?  O_TRUNC  : 0) | (flags.nonblock? O_NONBLOCK : 0)
		| O_NOCTTY;

	f= open(path.data, open_flags, 0600);
	if (f < 0)
		return END_CMD( ctl_notify_error(ctl, "Open failed: %s", strerror(errno)) );

	fd= fd_new_file(fdname, f, flags, path);
	if (!fd) {
		close(f);
		return END_CMD( ctl_notify_error(ctl, "Unable to create named file descriptor") );
	}

	ctl_notify_fd_state(NULL, fd);
	return END_CMD(true);
}

/** Run all processing needed for the controller for this time slice
 * This function is mainly a wrapper that repeatedly executes the current state until
 * the state_fn returns false.  We then flush buffers and decide what to wake on.
 */
void ctl_run(wake_t *wake) {
	int i;
	// list is very small, so "allocated" is any client with non-null state
	for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++) {
		if (client[i].state_fn) {
			controller_t *ctl= &client[i];
			// if anything in output buffer, try writing it
			if (ctl->send_buf_pos)
				ctl_flush_outbuf(ctl);
			// Run iterations of state machine while state returns true
			log_debug("ctl state = %s", ctl_get_state_name(ctl->state_fn));
			while (ctl->state_fn(ctl)) {
				log_debug("ctl state = %s", ctl_get_state_name(ctl->state_fn));
			}
			// Note: it is possible for ctl to have been destroyed (null state_fn), here.
			log_debug("ctl state = %s", ctl_get_state_name(ctl->state_fn));
		}
	}
}

/** Flush all output buffers of controller objects
 * This can't be part of ctl_run because services get handled after the controller,
 * and might generate notifications for the controllers.
 */
void ctl_flush(wake_t *wake) {
	int i;
	// list is very small, so "allocated" is any client with non-null state
	for (i= 0; i < CONTROLLER_MAX_CLIENTS; i++) {
		if (client[i].state_fn) {
			controller_t *ctl= &client[i];
			// If anything was left un-written, wake on writable pipe
			// Also, set/check timeout for writes
			if (ctl->send_fd >= 0 && ctl->send_buf_pos > 0) {
				if (!ctl_flush_outbuf(ctl)) {
					// If this is the first write which has not succeeded, then we set the
					// timestamp for the write timeout.
					if (!ctl->send_abort_ts) {
						ctl->send_abort_ts= wake->now + CONTROLLER_WRITE_TIMEOUT;
						// we use 0 as a "not stalled" flag, so if the actual timestamp is 0 fudge it to 1
						if (!ctl->send_abort_ts) ctl->send_abort_ts++;
					}
					else if (wake->now - ctl->send_abort_ts > 0) {
						ctl_dtor(ctl); // destroy client
						continue;      // next client
					}
					
					FD_SET(ctl->send_fd, &wake->fd_write);
					FD_SET(ctl->send_fd, &wake->fd_err);
					if (ctl->send_fd > wake->max_fd)
						wake->max_fd= ctl->send_fd;
					if (wake->next - ctl->send_abort_ts > 0)
						wake->next= ctl->send_abort_ts;
				}
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
}

// Read more controller input from recv_fd
bool ctl_read_more(controller_t *ctl) {
	int n;
	if (ctl->recv_fd < 0 || ctl->recv_buf_pos >= CONTROLLER_RECV_BUF_SIZE)
		return false;
	n= read(ctl->recv_fd, ctl->recv_buf + ctl->recv_buf_pos, CONTROLLER_RECV_BUF_SIZE - ctl->recv_buf_pos);
	if (n <= 0) {
		if (n == 0) {
			// EOF.  Close file descriptor
			close(ctl->recv_fd);
			ctl->recv_fd= -1;
		}
		else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
			log_error("read(controller pipe): %s (%d)", strerror(errno), errno);
		return false;
	}
	ctl->recv_buf_pos += n;
	return true;
}

// Try to write data to the controller script, nonblocking (or possibly stdout)
// Return true if the message was queued, or false if it can't be written.
// If the caller can't queue the event that generated the message, they need to call
//  ctl_write_overflow().
// If ctl is NULL, then all controllers with send_fd will be notified, assuming
//  they aren't in an overflow condition.  In this case, it always returns true.
bool ctl_write(controller_t *single_dest, const char *fmt, ... ) {
	controller_t * dest[CONTROLLER_MAX_CLIENTS];
	int dest_n, i;
	
	// Either send one message, or iterate all clients
	if (single_dest) {
		if (!single_dest->state_fn || single_dest->send_fd < 0 || single_dest->send_overflow)
			return false;
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
			if (!single_dest)
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
	return !single_dest || msg_data;
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
				ctl->send_abort_ts= 0;
				memmove(ctl->send_buf, ctl->send_buf + n, ctl->send_buf_pos);
			}
			else {
				log_debug("controller outbuf write failed: %s (%d)", strerror(errno), errno);
				// we have an error, or the write would block.
				return false;
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

bool ctl_notify_signal(controller_t *ctl, int sig_num) {
	return ctl_write(NULL, "signal %d %s\n", sig_num, sig_name(sig_num));
}

bool ctl_notify_svc_state(controller_t *ctl, const char *name, int64_t up_ts, int64_t reap_ts, int wstat, pid_t pid) {
	log_trace("ctl_notify_svc_state(%s, %lld, %lld, %d, %d)", name, up_ts, reap_ts, pid, wstat);
	if (!up_ts)
		return ctl_write(ctl, "service.state	%s	down\n", name);
	else if ((up_ts - wake->now) >= 0 && !pid)
		return ctl_write(ctl, "service.state	%s	starting	%d\n", name, up_ts>>32);
	else if (!reap_ts)
		return ctl_write(ctl, "service.state	%s	up	%d	pid	%d\n", name, up_ts>>32, (int) pid);
	else if (WIFEXITED(wstat))
		return ctl_write(ctl, "service.state	%s	down	%d	exit	%d	uptime %d	pid	%d\n",
			name, (int)(reap_ts>>32), WEXITSTATUS(wstat), (int)((reap_ts-up_ts)>>32), (int) pid);
	else
		return ctl_write(ctl, "service.state	%s	down	%d	signal	%d=%s	uptime %d	pid	%d\n",
			name, (int)(reap_ts>>32), WTERMSIG(wstat), sig_name(WTERMSIG(wstat)),
			(int)((reap_ts-up_ts)>>32), (int) pid);
}

bool ctl_notify_svc_meta(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.meta	%s	%s\n", name, tsv_fields);
}

bool ctl_notify_svc_argv(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.args	%s	%s\n", name, tsv_fields);
}

bool ctl_notify_svc_fds(controller_t *ctl, const char *name, const char *tsv_fields) {
	return ctl_write(ctl, "service.fds	%s	%s\n", name, tsv_fields);
}

bool ctl_notify_fd_state(controller_t *ctl, fd_t *fd) {
	fd_t *peer;
	const char *name= fd_get_name(fd);
	fd_flags_t flags= fd_get_flags(fd);
	
	if (flags.pipe) {
		peer= fd_get_pipe_peer(fd);
		return ctl_write(ctl, "fd.state	%s	pipe_%s	%s\n",
			name, flags.write? "to":"from", peer? fd_get_name(peer) : "?");
	}
	else if (flags.special)
		return ctl_write(ctl, "fd.state	%s	special	%s\n", name, fd_get_file_path(fd));
	else {
		return ctl_write(ctl, "fd.state	%s	file	%s%s%s%s%s%s	%s",
			(flags.write? (flags.read? "read,write":"write"):"read"),
			(flags.append? ",append":""), (flags.create? ",create":""),
			(flags.trunc? ",trunc":""), (flags.nonblock? ",nonblock":""),
			(flags.mkdir? ",mkdir":""), fd_get_file_path(fd));
	}
}
