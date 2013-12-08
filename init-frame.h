#ifndef INIT_FRAME_H
#define INIT_FRAME_H

// Get CLOCK_MONOTONIC microseconds as int64_t
int64_t gettime_us();

typedef struct wake_s {
	fd_set fd_read, fd_write, fd_err;
	int max_fd;
	// all times in microseconds
	// times might wrap!  never compare times with > >= < <=, only differences.
	int64_t now;  // current time according to main loop
	int64_t next; // time when we next need to process something
} wake_t;

extern int main_terminate;

//----------------------------------------------------------------------------
// controller.c interface

struct controller_s;
typedef struct controller_s controller_t;

// Queue a message to the controller, possibly overflowing the output buffer
// and requiring a state reset event.
void ctl_queue_message(char *msg);

// Handle state transitions based on communication filehandles to the controller
void ctl_run(controller_t *ctl);

//----------------------------------------------------------------------------
// service.c interface

struct service_s;
typedef struct service_s service_t;

// Set metadata for a service.  Creates the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_meta(const char *name, const char *tsv_fields);

// Set args for a service.  Creates the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_argv(const char *name, const char *tsv_fields);

// Set env for a service.  Created the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_env(const char *name, const char *tsv_fields);

// Set file descriptors for a service.  Create the service if it doesn't exist.
// Fails if the metadata + env + argv + fd is longer than the allocated buffer.
bool svc_set_fd(const char *name, const char *tsv_fields);

// update service state machine when requested to start
void svc_handle_exec(service_t *svc);

// update service state machine after PID is reaped
void svc_handle_reap(service_t *svc, int wstat);

// run an iteration of the state machine for the service
void svc_run(service_t *svc, wake_t *wake);

// Initialize the service pool from a static chunk of memory
void svc_build_pool(void *buffer, int service_count, int size_each);

// Lookup services by attributes
service_t *svc_by_name(const char *name, bool create);
service_t *svc_by_pid(pid_t pid);

// Deallocate service struct
void svc_delete(service_t *svc);

//----------------------------------------------------------------------------
// fd.c interface

struct fd_s;
typedef struct fd_s fd_t;

// Open a pipe from one named FD to another
// returns a ref to the write-end, which has a pointer to the read-end.
fd_t * fd_pipe(const char *name1, const char *name2);

// Open a file on the given name, possibly closing a handle by that name
fd_t * fd_open(const char *name, const char *path, const char *opts);

// Close a named handle
bool fd_close(const char *name);

fd_t * fd_by_name(const char *name);
fd_t * fd_by_fd(int fd);

//----------------------------------------------------------------------------
// signal.c interface

// Initialize signal-related things
void sig_setup();

// Deliver any caught signals as messages to the controller,
// and set a wake for reads on the signal fd
void sig_run(wake_t *wake);

#endif