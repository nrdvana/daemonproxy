/* daemonproxy.h - unified header for entire project
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#ifndef DAEMONPROXY_H
#define DAEMONPROXY_H

struct fd_s;
typedef struct fd_s fd_t;

struct service_s;
typedef struct service_s service_t;

struct controller_s;
typedef struct controller_s controller_t;

// Get CLOCK_MONOTONIC time as 32.32 fixed-point number
int64_t gettime_mon_frac();

#define EXIT_NO_OP                1
#define EXIT_BAD_OPTIONS          2
#define EXIT_INVALID_ENVIRONMENT  3
#define EXIT_BROKEN_PROGRAM_STATE 4
#define EXIT_IMPOSSIBLE_SCENARIO  5
#define EXIT_TERMINATE            6
void fatal(int exitcode, const char * msg, ...);

typedef struct wake_s {
	fd_set fd_read, fd_write, fd_err;
	int max_fd;
	// all times in microseconds
	// times might wrap!  never compare times with > >= < <=, only differences.
	int64_t now;  // current time according to main loop
	int64_t next; // time when we next need to process something
} wake_t;

#define STRSEG(s) ((strseg_t){ s, strlen(s) })
typedef struct strseg_s {
	const char *data;
	int len;
} strseg_t;

bool strseg_tok_next(strseg_t *string_inout, char sep, strseg_t *tok_out);
int  strseg_cmp(strseg_t a, strseg_t b);
bool strseg_atoi(strseg_t *str, int64_t *int_out);
bool strseg_parse_size(strseg_t *string, int64_t *val);

//----------------------------------------------------------------------------
// Log.c interface

#define LOG_LEVEL_FATAL 3
#define LOG_LEVEL_ERROR 2
#define LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 0
#define LOG_LEVEL_DEBUG -1
#define LOG_LEVEL_TRACE -2
#define LOG_FILTER_NONE -3

void log_init();
bool log_write(int level, const char * msg, ...);
void log_run();

int  log_get_fd();
void log_fd_reset();
void log_fd_set_name(strseg_t name);

extern int log_filter;
void log_set_filter(int level);
const char * log_level_name(int level);
bool log_level_by_name(strseg_t name, int *lev);

#define log_error(args...) log_write(LOG_LEVEL_ERROR, args)
#define log_warn(args...)  log_write(LOG_LEVEL_WARN,  args)
#define log_info(args...)  log_write(LOG_LEVEL_INFO,  args)
#define log_debug(args...) log_write(LOG_LEVEL_DEBUG, args)
#ifndef NDEBUG
#define log_trace(args...) log_write(LOG_LEVEL_TRACE, args)
#else
#define log_trace(args...) do {} while (0)
#endif

extern bool     opt_daemonize;
extern int      opt_fd_pool_count;
extern int      opt_fd_pool_size_each;
extern int      opt_svc_pool_count;
extern int      opt_svc_pool_size_each;
extern const char * opt_socket_path;
extern const char * opt_config_file;
extern bool     opt_interactive;
extern bool     opt_exec_on_exit;
extern strseg_t opt_exec_on_exit_args;
extern bool     opt_mlockall;
extern int64_t  opt_terminate_guard;

void parse_opts(char **argv);
bool set_exec_on_exit(strseg_t args);

extern bool    main_terminate;
extern int     main_exitcode;
extern wake_t *wake;
extern void main_notify_controller_freed(controller_t *ctl);

const int     version_major;
const int     version_minor;
const int     version_release;
const char *  version_suffix;
const time_t  version_build_ts;
const char *  version_git_head;
const bool    version_git_dirty;

void create_missing_dirs(char *path);

void control_socket_init();
void control_socket_run();
bool control_socket_start(strseg_t path);
void control_socket_stop();

//----------------------------------------------------------------------------
// controller.c interface

// Initialize controller state machine.
void ctl_init();
controller_t * ctl_new(int recv_fd, int send_fd);
controller_t * ctl_alloc();
void ctl_free(controller_t *ctl);
bool ctl_ctor(controller_t *ctl, int recv_fd, int send_fd);
void ctl_dtor(controller_t *ctl);

void ctl_set_auto_final_newline(controller_t *ctl, bool enable);

// Queue a message to the controller, possibly overflowing the output buffer
// and requiring a state reset event.
// If no controller is running, a sighup message will cause the config file to be re-loaded.
bool ctl_write(controller_t *ctl, const char *msg, ... );
bool ctl_notify_signal(controller_t *ctl, int sig_num, int64_t sig_ts, int count);
bool ctl_notify_svc_state(controller_t *ctl, const char *name, int64_t up_ts, int64_t reap_ts, pid_t pid, int wstat);
bool ctl_notify_svc_meta(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_argv(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_fds(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_autostart(controller_t *ctl, const char *name, int64_t interval, const char *tsv_triggers);
bool ctl_notify_fd_state(controller_t *ctl, fd_t *fd);
#define ctl_notify_error(ctl, msg, ...) (ctl_write(ctl, "error\t" msg "\n", ##__VA_ARGS__))

// Handle state transitions based on communication with the controller
void ctl_run(wake_t *wake);
void ctl_flush(wake_t *wake);

//----------------------------------------------------------------------------
// service.c interface

void svc_init();
// Initialize the service pool
bool svc_preallocate(int service_count, int data_size_each);

const char * svc_get_name(service_t *svc);
bool svc_check_name(strseg_t name);

pid_t   svc_get_pid(service_t *svc);
int     svc_get_wstat(service_t *svc);
int64_t svc_get_up_ts(service_t *svc);
int64_t svc_get_reap_ts(service_t *svc);

// Set args for a service.  Creates the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_argv(service_t *svc, strseg_t tsv_fields);
const char * svc_get_argv(service_t *svc);

// Set file descriptors for a service.  Create the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_fds(service_t *svc, strseg_t tsv_fields);
const char * svc_get_fds(service_t *svc);

int64_t svc_get_restart_interval(service_t *svc);
bool svc_set_restart_interval(service_t *svc, int64_t interval);

const char * svc_get_triggers(service_t *svc);
bool svc_set_triggers(service_t *svc, strseg_t triggers_tsv);

// update service state machine when requested to start
bool svc_handle_start(service_t *svc, int64_t when);

// update service state machine after PID is reaped
void svc_handle_reaped(service_t *svc, int wstat);

// run an iteration of the state machine for the service
void svc_run(service_t *svc, wake_t *wake);
void svc_run_active(wake_t *wake);

// Lookup services by attributes
service_t * svc_by_name(strseg_t name, bool create);
service_t * svc_by_pid(pid_t pid);
service_t * svc_iter_next(service_t *current, const char *from_name);

bool svc_send_signal(service_t *svc, int sig, bool group);

// Deallocate service struct
void svc_delete(service_t *svc);

//----------------------------------------------------------------------------
// fd.c interface

typedef struct fd_flags_s {
	bool read: 1,
		write: 1,
		create: 1,
		append: 1,
		mkdir: 1,
		trunc: 1,
		nonblock: 1,
		pipe: 1,
		special: 1,
		is_const: 1;
} fd_flags_t;

extern const int fd_min_obj_size, fd_max_obj_size;
extern int fd_dev_null;

// Initialize the fd pool from a static chunk of memory
void fd_init();
bool fd_preallocate(int count, int data_size_each);
bool fd_init_special_handles();

const char* fd_get_name(fd_t *fd);
int         fd_get_fdnum(fd_t *fd);
void        fd_set_fdnum(fd_t *fd, int fdnum);
fd_flags_t  fd_get_flags(fd_t *fd);
const char* fd_get_file_path(fd_t *fd);
fd_t *      fd_get_pipe_peer(fd_t *fd);

// Open a pipe from one named FD to another
// returns a ref to the write-end, which has a pointer to the read-end.
fd_t * fd_new_pipe(strseg_t name1, int fd1, strseg_t name2, int fd2);

// Open a file on the given name, possibly closing a handle by that name
fd_t * fd_new_file(strseg_t name, int fdnum, fd_flags_t flags, strseg_t path);

void fd_delete(fd_t *fd);

#define fd_check_name svc_check_name
fd_t * fd_by_name(strseg_t name);
fd_t * fd_by_num(int fdnum);
fd_t * fd_by_fd(int fd);
fd_t * fd_iter_next(fd_t *current, const char *from_name);

//----------------------------------------------------------------------------
// signal.c interface

// Initialize signal-related things
void sig_init();

// Enable or disable receipt of signals
void sig_enable(bool enable);

// Reset signal handling after fork() before exec()
void sig_reset_for_exec();

// Get next event based on previous timestamp
bool sig_get_new_events(int64_t since_ts, int *sig_out, int64_t *ts_out, int *count_out);

// Record that a signal has been dealt with by subtracting from its count
void sig_mark_seen(int signal, int count);

// Main loop processing for signals.
void sig_run();

const char * sig_name_by_num(int sig_num);
int sig_num_by_name(strseg_t name);


#ifdef NDEBUG
#define svc_check(svc)
#else
void svc_check(service_t *svc);
#endif

#endif
