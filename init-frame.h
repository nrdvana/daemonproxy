#ifndef INIT_FRAME_H
#define INIT_FRAME_H

// Get CLOCK_MONOTONIC time as 32.32 fixed-point number
int64_t gettime_mon_frac();

void fatal(int exitcode, const char* msg, ...);

typedef struct wake_s {
	fd_set fd_read, fd_write, fd_err;
	int max_fd;
	// all times in microseconds
	// times might wrap!  never compare times with > >= < <=, only differences.
	int64_t now;  // current time according to main loop
	int64_t next; // time when we next need to process something
} wake_t;

typedef struct strseg_s {
	const char *data;
	int len;
} strseg_t;

typedef void log_fn_t(const char *msg, ...);

extern log_fn_t *log_error, *log_warn, *log_info, *log_debug, *log_trace, log_null;

extern bool main_terminate;
extern wake_t *wake;

//----------------------------------------------------------------------------
// controller.c interface

struct controller_s;
typedef struct controller_s controller_t;

// Initialize controller state machine.
void ctl_init();
controller_t * ctl_new(int recv_fd, int send_fd);
void ctl_set_auto_final_newline(controller_t *ctl, bool enable);

// Queue a message to the controller, possibly overflowing the output buffer
// and requiring a state reset event.
// If no controller is running, a sighup message will cause the config file to be re-loaded.
bool ctl_write(controller_t *ctl, const char *msg, ... );
bool ctl_notify_signal(controller_t *ctl, int sig_num);
bool ctl_notify_svc_state(controller_t *ctl, const char *name, int64_t up_ts, int64_t reap_ts, pid_t pid, int wstat);
bool ctl_notify_svc_meta(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_argv(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_svc_fds(controller_t *ctl, const char *name, const char *tsv_fields);
bool ctl_notify_fd_state(controller_t *ctl, const char *name, const char *file_path, const char *pipe_read, const char *pipe_write);
#define ctl_notify_error(ctl, msg, ...) (ctl_write(ctl, "error: " msg "\n", ##__VA_ARGS__))

// Handle state transitions based on communication with the controller
void ctl_run(wake_t *wake);
void ctl_flush(wake_t *wake);

//----------------------------------------------------------------------------
// service.c interface

struct service_s;
typedef struct service_s service_t;
extern const int min_service_obj_size;

// Initialize the service pool
void svc_init(int service_count, int size_each);

const char * svc_get_name(service_t *svc);
bool svc_check_name(const char *name);

pid_t   svc_get_pid(service_t *svc);
int     svc_get_wstat(service_t *svc);
int64_t svc_get_up_ts(service_t *svc);
int64_t svc_get_reap_ts(service_t *svc);

// Set metadata for a service.  Creates the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_meta(service_t *svc, const char *tsv_fields);
const char * svc_get_meta(service_t *svc);

// Set args for a service.  Creates the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_argv(service_t *svc, const char *tsv_fields);
const char * svc_get_argv(service_t *svc);

// Set env for a service.  Created the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
//bool svc_set_env(const char *name, const char *tsv_fields);

// Set file descriptors for a service.  Create the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_fds(service_t *svc, const char *tsv_fields);
const char * svc_get_fds(service_t *svc);

// update service state machine when requested to start
bool svc_handle_start(service_t *svc, int64_t when);

// update service state machine after PID is reaped
void svc_handle_reaped(service_t *svc, int wstat);

// run an iteration of the state machine for the service
void svc_run(service_t *svc, wake_t *wake);
void svc_run_active(wake_t *wake);

// Lookup services by attributes
service_t * svc_by_name(const char *name, bool create);
service_t * svc_by_pid(pid_t pid);
service_t * svc_iter_next(service_t *current, const char *from_name);

// Deallocate service struct
void svc_delete(service_t *svc);

//----------------------------------------------------------------------------
// fd.c interface

struct fd_s;
typedef struct fd_s fd_t;
extern const int min_fd_obj_size;

// Initialize the fd pool from a static chunk of memory
void fd_init(int fd_count, int size_each);

const char* fd_get_name(fd_t *);
int         fd_get_fdnum(fd_t *fd);
const char* fd_get_file_path(fd_t *);
const char* fd_get_pipe_read_end(fd_t *fd);
const char* fd_get_pipe_write_end(fd_t *fd);

// Open a pipe from one named FD to another
// returns a ref to the write-end, which has a pointer to the read-end.
fd_t * fd_pipe(const char *name1, const char *name2);

// Open a file on the given name, possibly closing a handle by that name
fd_t * fd_open(const char *name, char *path, char *opts);

// Manually inject a named FD into the lookup table.  allow_close determines whether users can remove it.
fd_t * fd_assign(const char *name, int fd, bool is_const, const char *description);

// Close a named handle
bool fd_close(const char *name);

bool fd_notify_state(fd_t *fd);

fd_t * fd_by_name(strseg_t name, bool create);
fd_t * fd_by_fd(int fd);
fd_t * fd_iter_next(fd_t *current, const char *from_name);

void fd_delete(fd_t *fd);

//----------------------------------------------------------------------------
// signal.c interface

// Initialize signal-related things
void sig_init();

// Deliver any caught signals as messages to the controller,
// and set a wake for reads on the signal fd
void sig_run(wake_t *wake);

const char * sig_name(int sig_num);

#ifdef NDEBUG
#define svc_check(svc)
#else
void svc_check(service_t *svc);
#endif

#endif
