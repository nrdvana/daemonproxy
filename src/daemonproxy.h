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

//----------------------------------------------------------------------------
// strseg.c interface

#define STRSEG(s) ((strseg_t){ s, strlen(s) })
typedef struct strseg_s {
	const char *data;
	int len;
} strseg_t;

// extract one token from string_input (delimited by sep) and place it into tok_out
bool strseg_tok_next(strseg_t *string_inout, char sep, strseg_t *tok_out);

// strcmp() for strseg_t structs
int  strseg_cmp(strseg_t a, strseg_t b);

// parse integer, extracting digits from str and returning true if got any
bool strseg_atoi(strseg_t *str, int64_t *int_out);

// parse integer with size suffix, returning true if extracted any digits
bool strseg_parse_size(strseg_t *string, int64_t *val);

//----------------------------------------------------------------------------
// daemonproxy.c interface

// Get CLOCK_MONOTONIC time as 32.32 fixed-point number.
// All timestamps and durations in daemonproxy use this format.
int64_t gettime_mon_frac();

#define EXIT_NO_OP                1
#define EXIT_BAD_OPTIONS          2
#define EXIT_INVALID_ENVIRONMENT  3
#define EXIT_BROKEN_PROGRAM_STATE 4
#define EXIT_IMPOSSIBLE_SCENARIO  5
#define EXIT_TERMINATE            6

// Terminate program with message, unless exec-on-exit feature, or exit-guard feature
// says otherwise.
void fatal(int exitcode, const char * msg, ...);

typedef struct wake_s {
	fd_set fd_read, fd_write, fd_err;
	int max_fd;
	// times might wrap! (in theory) never compare times with > >= < <=, only differences.
	int64_t now;  // current time according to main loop
	int64_t next; // time when we next need to process something
} wake_t;

extern wake_t *wake;
extern bool    main_terminate;
extern int     main_exitcode;

// callback type function so main can handle the termination of a controller
void main_notify_controller_freed(controller_t *ctl);

// util function to perform "mkdir -p"
void create_missing_dirs(char *path);


//----------------------------------------------------------------------------
// version.autogen.c interface

extern const int     version_major;
extern const int     version_minor;
extern const int     version_release;
extern const char *  version_suffix;
extern const time_t  version_build_ts;
extern const char *  version_git_head;
extern const bool    version_git_dirty;

//----------------------------------------------------------------------------
// options.c interface

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

// Parse main's argv[] to find option settings
void parse_opts(char **argv);

// Assign a new value to the exec-on-exit option
bool set_exec_on_exit(strseg_t args_tsv);

//----------------------------------------------------------------------------
// log.c interface

#define LOG_LEVEL_FATAL 3
#define LOG_LEVEL_ERROR 2
#define LOG_LEVEL_WARN 1
#define LOG_LEVEL_INFO 0
#define LOG_LEVEL_DEBUG -1
#define LOG_LEVEL_TRACE -2
#define LOG_FILTER_NONE -3

// Initialize module
void log_init();

// Write message to log, printf() style, at a given log level
bool log_write(int level, const char * msg, ...);

// Perform anything needed per main loop iteration related to logging
void log_run();

// Find out what fd number the log is writing
int  log_get_fd();

// Tell logger the named destination FD might have changed
void log_fd_reset();

// Redirect logging to new named FD
void log_fd_set_name(strseg_t name);

extern int log_filter;

// Set filtering level for logger
void log_set_filter(int level);

// Convert log level to level name
const char * log_level_name(int level);

// Parse log level name into integer
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

//----------------------------------------------------------------------------
// control-socket.c interface

// Initialize module
void control_socket_init();

// Run one iteration of control-socket module (accept new connections)
void control_socket_run();

// Create the controller socket
bool control_socket_start(strseg_t path);

// Remove a previously created controller socket
void control_socket_stop();

//----------------------------------------------------------------------------
// controller.c interface

// Initialize controller module
void ctl_init();

// Create new controller on specified file handles
controller_t * ctl_new(int recv_fd, int send_fd);

// Allocate a controller
controller_t * ctl_alloc();

// Free an allocated controller which was not constructed
void ctl_free(controller_t *ctl);

// Construct a pre-allocated controller on specified file handles
bool ctl_ctor(controller_t *ctl, int recv_fd, int send_fd);

// Destroy a controller
void ctl_dtor(controller_t *ctl);

// Toggle flag of whether partial line should be treated as complete command
void ctl_set_auto_final_newline(controller_t *ctl, bool enable);

// Queue a message to the controller, possibly overflowing the output buffer
// and requiring a state reset event.
bool ctl_write(controller_t *ctl, const char *msg, ... );

// Notify functions are simply a way to keep all the event "printf" statements in one place.
bool ctl_notify_signal(controller_t *ctl, int sig_num, int64_t sig_ts, int count);
bool ctl_notify_svc_state(controller_t *ctl, const char *name, int64_t up_ts, int64_t reap_ts, pid_t pid, int wstat);
bool ctl_notify_svc_meta(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_argv(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_fds(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_auto_up(controller_t *ctl, const char *name, int64_t interval, const char *tsv_triggers);
bool ctl_notify_fd_state(controller_t *ctl, fd_t *fd);
#define ctl_notify_error(ctl, msg, ...) (ctl_write(ctl, "error\t" msg "\n", ##__VA_ARGS__))

// Run all active controller state machines
void ctl_run();

//----------------------------------------------------------------------------
// service.c interface

void svc_init();

// Initialize the service pool
bool svc_preallocate(int service_count, int data_size_each);

// get name of service, NUL terminated
const char * svc_get_name(service_t *svc);

// validate name for use as service name
bool svc_check_name(strseg_t name);

// simple getter functions
pid_t   svc_get_pid(service_t *svc);
int     svc_get_wstat(service_t *svc);
int64_t svc_get_up_ts(service_t *svc);
int64_t svc_get_reap_ts(service_t *svc);
int64_t svc_get_restart_interval(service_t *svc);

// Set args for a service. Fails if unable to allocate the needed space
bool svc_set_argv(service_t *svc, strseg_t tsv_fields);

// Return TSV string of args
const char * svc_get_argv(service_t *svc);

// Set file descriptors for a service. Fails if unable to allocate the needed space
bool svc_set_fds(service_t *svc, strseg_t tsv_fields);

// Return TSV string of fds
const char * svc_get_fds(service_t *svc);

// Set restart interval (used if service has an auto_up trigger)
bool svc_set_restart_interval(service_t *svc, int64_t interval);

// Return TSV string of auto_up values
const char * svc_get_triggers(service_t *svc);

// Set TSV string of triggers for the auto_up feature
bool svc_set_triggers(service_t *svc, strseg_t triggers_tsv);

// Tell service state machine to start at specified time
bool svc_handle_start(service_t *svc, int64_t when);

// Tell service state machine it has been reaped
void svc_handle_reaped(service_t *svc, int wstat);

// Run an iteration of the state machine for the service
void svc_run(service_t *svc);

// Run all services which need running
void svc_run_active();

// Lookup services by attributes
service_t * svc_by_name(strseg_t name, bool create);

// Lookup service by PID (IFF it is running)
service_t * svc_by_pid(pid_t pid);

// Iterate list of services, either from a previous obj, or from a previous name
service_t * svc_iter_next(service_t *current, const char *from_name);

// Send signal to service IFF running.  If group is true, send to process group.
bool svc_send_signal(service_t *svc, int sig, bool group);

// Deallocate service struct
void svc_delete(service_t *svc);

// If debugging, svc_check routine performs sanity check on service object.
#ifdef NDEBUG
#define svc_check(svc)
#else
void svc_check(service_t *svc);
#endif

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

void fd_init();

// Initialize the fd pool from a static chunk of memory
bool fd_preallocate(int count, int data_size_each);

// Initialize the special named constants like "control.event"
bool fd_init_special_handles();

// initialized by fd_init_special_handles()
extern int fd_dev_null;

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

// Close and free a FD object
void fd_delete(fd_t *fd);

// Validate a name for use as a FD object (same rules as for services)
#define fd_check_name svc_check_name

// Find a FD by name, NULL if not found
fd_t * fd_by_name(strseg_t name);

// Find a FD by file descriptor number (not indexed, could be slow)
fd_t * fd_by_num(int fdnum);

// Iterate list of FDs, either from a previous obj, or from a previous name
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

// Return signal constant name (minus the "SIG" prefix)
const char * sig_name_by_num(int sig_num);

// Return a number for a signal constant name (with or without "SIG" prefix)
int sig_num_by_name(strseg_t name);

#endif
