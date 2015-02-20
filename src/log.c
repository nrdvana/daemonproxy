/* log.c - routines for logging
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

int  log_filter= LOG_LEVEL_DEBUG;
char log_dest_fd_name_buf[NAME_BUF_SIZE];
strseg_t log_dest_fd_name= (strseg_t){ log_dest_fd_name_buf, 0 };
char log_buffer[1024];
int  log_buf_pos= 0;
int  log_fd= 2;
int  log_msg_lost= 0;

const char * log_level_names[]= { "none", "trace", "debug", "info", "warning", "error", "fatal" };

const char * log_level_name(int level) {
	if (level >= LOG_FILTER_NONE && level <= LOG_LEVEL_FATAL)
		return log_level_names[level - LOG_FILTER_NONE];
	return "unknown";
}

static bool log_flush();
static bool log_fd_attach();

void log_init() {
	log_fd= 2;
	log_fd_attach();
}

int log_get_fd() {
	return log_fd;
}

void log_fd_reset() {
	if (log_fd >= 0) {
		FD_CLR(log_fd, &wake->fd_write);
		log_fd= -1;
	}
}

void log_fd_set_name(strseg_t name) {
	log_fd_reset();

	assert(name.len < sizeof(log_dest_fd_name_buf));
	memcpy(log_dest_fd_name_buf, name.data, name.len);
	log_dest_fd_name_buf[name.len]= '\0';
	log_dest_fd_name.len= name.len;
	
	log_fd_attach();
}

static bool log_fd_attach() {
	fd_t *fd;
	// If the FD the log was going to got closed, log_fd is set to -1.
	// To resume logging, we check to see if the named FD has become available
	// again.
	// Note: log module gets initialized before fd module, but log_dest_fd_name
	// doesn't get set until afterward.
	if (log_fd < 0 && log_dest_fd_name.len > 0 && (fd= fd_by_name(log_dest_fd_name)))
		log_fd= fd_get_fdnum(fd);

	if (log_fd < 0)
		return false;
	
	if (log_buf_pos > 0)
		FD_SET(log_fd, &wake->fd_write);
	else
		FD_CLR(log_fd, &wake->fd_write);
	return true;
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

bool log_write(int level, const char *msg, ...) {
	char *p, *limit;
	int n;
	va_list val;
	
	if (log_filter >= level)
		return true;

	if (log_msg_lost) {
		log_msg_lost++;
		return false;
	}

	p= log_buffer + log_buf_pos;
	limit= log_buffer + sizeof(log_buffer);
	
	n= snprintf(p , limit - p, "%s: ", log_level_name(level));
	if (n >= limit - p) {
		log_msg_lost++;
		return false;
	}
	p+= n;
	va_start(val, msg);
	n= vsnprintf(p, limit - p, msg, val);
	va_end(val);
	if (n >= limit - p) {
		log_msg_lost++;
		return false;
	}
	p+= n;
	*p++ = '\n';
	log_buf_pos= p - log_buffer;
	log_flush();
	return true;
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
	if (log_fd < 0 && !log_fd_attach())
		return;

	if (FD_ISSET(log_fd, &wake->fd_write)) {
		FD_CLR(log_fd, &wake->fd_write);
		log_flush();
	}
}

static bool log_flush() {
	int n;
	struct itimerval t;
	// If we failed to write to the log once, don't try again until the
	//  main loop calls log_run.
	if (log_fd < 0 || FD_ISSET(log_fd, &wake->fd_write))
		return false;
	
	while (log_buf_pos > 0) {
		// Use timer+signal to break hung write()s.  Not good to set to nonblocking
		//  mode because it might be the terminal, and can't do nonblocking if it's
		//  an actual file, anyway.
		memset(&t, 0, sizeof(t));
		t.it_value.tv_usec= 100000;
		setitimer(ITIMER_REAL, &t, NULL);
		
		n= write(log_fd, log_buffer, log_buf_pos);
		
		memset(&t, 0, sizeof(t));
		setitimer(ITIMER_REAL, &t, NULL);
		
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				FD_SET(log_fd, &wake->fd_write);
				if (log_fd > wake->max_fd)
					wake->max_fd= log_fd;
			}
			return false;
		}
		if (n > 0 && n < log_buf_pos)
			memmove(log_buffer, log_buffer + n, log_buf_pos - n);
		log_buf_pos -= n;
		
		// If messages were lost, and we've freed up some buffer space,
		// see if we can append the message saying that we lost messages
		if (log_msg_lost) {
			n= snprintf(log_buffer + log_buf_pos, sizeof(log_buffer) - log_buf_pos, "warning: lost %d log messages\n", log_msg_lost);
			if (n < sizeof(log_buffer) - log_buf_pos) {
				log_buf_pos+= n;
				log_msg_lost= 0;
			}
		}
	}
	return true;
}

