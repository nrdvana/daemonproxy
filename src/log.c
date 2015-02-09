/* log.c - routines for logging
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

int  log_filter= LOG_LEVEL_DEBUG;
char log_buffer[1024];
int  log_msg_len= 0;
int  log_flush_pos= 0;
int  log_msg_lost= 0;
bool log_nonblock_safe= false;

const char * log_level_names[]= { "none", "trace", "debug", "info", "warning", "error", "fatal" };

const char * log_level_name(int level) {
	if (level >= LOG_FILTER_NONE && level <= LOG_LEVEL_FATAL)
		return log_level_names[level - LOG_FILTER_NONE];
	return "unknown";
}

bool log_level_by_name(strseg_t name, int *lev) {
	int i;
	for (i= LOG_FILTER_NONE; i <= LOG_LEVEL_FATAL; i++)
		if (0 == strseg_cmp(name, STRSEG(log_level_names[i - LOG_FILTER_NONE]))) {
			if (lev) *lev= i;
			return true;
		}
	return false;
}

static bool log_flush();

void log_init() {
}

// Stage a log message in the logging buffer.  Returns true if entire message
// was queued, or false if it didn't fit.  If it doesn't fit, we increment a
// count of "lost messages", and when the log becomes writable again we first
// write a message saying how many were lost.
//
// Note: NEVER CALL fatal() from this function or any sub-call.
//
bool log_write(int level, const char *msg, ...) {
	char *p, *limit;
	int n;
	va_list val;
	
	if (log_filter >= level) // is message level squelched?
		return true;

	// if we already have a full pipe and an un-flushed message, discard this message
	if (log_msg_len) {
		log_msg_lost++;
		return false;
	}

	// Prefix with log level, except for "info" which would be redundant.
	if (level != LOG_LEVEL_INFO) {
		n= snprintf(log_buffer, sizeof(log_buffer), "%s: ", log_level_name(level));
		log_msg_len+= n;
	}
	va_start(val, msg);
	n= vsnprintf(log_buffer + log_msg_len, sizeof(log_buffer) - log_msg_len, msg, val);
	va_end(val);
	if (log_msg_len < sizeof(log_buffer))
		log_buffer[log_msg_len]= "\n";
	log_msg_len+= n + 1;

	// If our sprintf would have overflowed, then mark this as a lost message
	// but, still try to flush it so the "lost message" message gets sent.
	if (log_msg_len > sizeof(log_buffer)) {
		log_msg_lost++;
		log_msg_len= 0;
		log_flush();
		return false;
	}
	else {
		log_flush();
		return true;
	}
}


void log_running_services() {
	service_t *svc= NULL;
	while ((svc= svc_iter_next(svc, ""))) {
		pid_t pid= svc_get_pid(svc);
		int64_t reap_ts= svc_get_reap_ts(svc);
		if (pid && !reap_ts) {
			log_warn("service '%s' running as pid %d", svc_get_name(svc), (int) pid);
		}
	}
}

void log_set_filter(int value) {
	log_filter= (value > LOG_LEVEL_FATAL)? LOG_LEVEL_FATAL
		: (value < LOG_FILTER_NONE)? LOG_FILTER_NONE
		: value;
}

void log_run() {
	if (log_w_fd > 0 && FD_ISSET(log_w_fd, &wake->fd_write)) {
		FD_CLR(log_w_fd, &wake->fd_write);
		log_flush();
	}
}

// Note: NEVER CALL fatal() from this function or any sub-call.
static bool log_flush() {
	int n;
	
	while (log_flush_pos < log_msg_len) {
		if (!log_nonblock_safe) alarm(1);
		n= write(2, log_buffer + log_flush_pos, log_msg_len - log_flush_pos);
		if (!log_nonblock_safe) alarm(0);
		
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				FD_SET(log_fd, &wake->fd_write);
				if (log_fd > wake->max_fd)
					wake->max_fd= log_fd;
			}
			return false;
		}
		log_flush_pos += n;
		
		// If messages were lost, and we're able to write again,
		// see if we can append the message saying that we lost messages
		if (log_msg_lost && sizeof(log_buffer) - log_msg_len > 0) {
			n= snprintf(log_buffer + log_msg_len, sizeof(log_buffer) - log_msg_len, "warning: lost %d log messages\n", log_msg_lost);
			if (n < sizeof(log_buffer) - log_msg_len) {
				log_msg_len+= n;
				log_msg_lost= 0;
			}
		}
	}
	// completely flushed.  reset buffer.
	log_flush_pos= log_msg_len= 0;
	return true;
}

