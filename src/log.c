#include "config.h"
#include "daemonproxy.h"

int  log_filter= LOG_LEVEL_DEBUG;
char log_buffer[1024];
int  log_buf_pos= 0;
int  log_fd= 2;
int  log_msg_lost= 0;

const char * log_level_names[]= { "none", "trace", "debug", "info", "warning", "error", "fatal" };

const char * log_level_name(int level) {
	if (level >= LOG_LEVEL_NONE && level <= LOG_LEVEL_FATAL)
		return log_level_names[level - LOG_LEVEL_NONE];
	return "unknown";
}

void log_init() {
	if (fcntl(log_fd, F_SETFL, O_NONBLOCK))
		log_error("unable to set stderr to nonblocking mode!  logging might block daemonproxy!");
}

bool log_level_by_name(strseg_t name, int *lev) {
	int i;
	for (i= LOG_LEVEL_NONE; i <= LOG_LEVEL_FATAL; i++)
		if (0 == strseg_cmp(name, STRSEG(log_level_names[i - LOG_LEVEL_NONE]))) {
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

void log_set_filter(int value) {
	log_filter= (value > LOG_LEVEL_FATAL)? LOG_LEVEL_FATAL
		: (value < LOG_LEVEL_TRACE-1)? LOG_LEVEL_TRACE-1
		: value;
}

bool log_flush() {
	int n;
	
	while (log_buf_pos > 0) {
		n= write(log_fd, log_buffer, log_buf_pos);
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

