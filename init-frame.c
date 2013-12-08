#include "config.h"
#include "Contained_RBTree.h"

// Describes a service, complete with metadata,
// argument list, environment modifications,
// file descriptor specification, and a state
// machine for watching the PID.

#define SVC_STATE_UNDEF         0
#define SVC_STATE_DOWN          1
#define SVC_STATE_START_PENDING 2
#define SVC_STATE_START         3
#define SVC_STATE_UP            4
#define SVC_STATE_REAPED        5

typedef struct service_s {
	int state;
	char buffer[CONFIG_SERVICE_DATA_BUF_SIZE];
	int meta_count, arg_count, env_count;
	RBTreeNode name_index_node;
	RBTreeNode pid_index_node;
	struct service_s **active_prev_ptr, *active_next;
	pid_t pid;
	bool auto_restart: 1;
	int wait_status;
	int64_t start_time;
	int64_t reap_time;
} service_t;

// Describes a named file handle
typedef struct fd_def_s {
	char name[CONFIG_FD_NAME_BUF_SIZE];
	RBTreeNode name_index_node;
	int fd;
} fd_t;

// Describes the parameters and state machine for interacting with the
// controller process.

#define CTL_STATE_UNDEF     0
#define CTL_STATE_CFGFILE   1
#define CTL_STATE_NONE      2
#define CTL_STATE_STARTED   3
#define CTL_STATE_SENDSTATE 4
#define CTL_STATE_RUNNING   5
#define CTL_STATE_REAPED    6

typedef struct controller_s {
	int  state;
	char exec_buf[CONFIG_SERVICE_EXEC_BUF_SIZE];
	int  arg_count, env_count;
	const char* config_file;
	int  recv_fd, send_fd;
	char in_buf[CONTROLLER_IN_BUF_SIZE];
	int  in_buf_len;
	char out_buf[CONTROLLER_OUT_BUF_SIZE];
	int  out_buf_len;
	bool out_buf_overflow;
} controller_t;

controller_t controller= {
	.state=    CTL_STATE_UNDEF,
	.exec_buf= CONTROLLER_DEFAULT_PATH "\0" CONTROLLER_DEFAULT_PATH,
	.arg_count= 2,
	.env_count= 0,
	.config_file= CONFIG_FILE_DEFAULT_PATH,
	.recv_fd= -1,
	.send_fd= -1,
	.in_buf_len= 0,
	.out_buf_len= 0,
	.out_buf_overflow= false,
};

// Allocated once, lazily, and then treated as a fixed-size allocation pool
service_t *svc_pool= NULL;
service_t *svc_active_list= NULL;
service_t *svc_free_list= NULL; // linked list re-using next_active and prev_active
int svc_active_count;
// Indexes and lists of services.
RBTreeNode service_by_name_index;
RBTreeNode service_by_pid_index;
service_t *service_free_list; 

// Allocated once, lazily, and then treated as a fixed-size allocation pool
fd_t *fd_pool= NULL;
RBTreeNode fd_by_name_index;
fd_t *fd_free_list;

// Signal-handling.  We watch SIGCHLD, SIGINT, SIGHUP, SIGTERM, SIGUSR1, SIGUSR2
int sig_wake_rd= -1;
int sig_wake_wr= -1;
int got_sigint= 0, got_sighup= 0, got_sigterm= 0, got_sigusr1= 0, got_sigusr2= 0;

bool terminate= false;
struct wake_s {
	fd_set fd_read, fd_write, fd_err;
	int max_fd;
	int64_t now, next;
} wake_t;

int64_t gettime_us();

// Initialize signal-related things
void sig_setup();

// Deliver any caught signals as messages to the controller
void sig_send_notifications();

// Parse options and apply to global vars; return false if fails
bool parse_opts(char **argv);

// Allocate memory for the service definitions, and call mlockall()
bool alloc_structures(int service_count, int fd_count);

// Open a pipe of $name, creating handles "$name.r" and "$name.w"
bool open_pipe(const char *name, int *rd_fd, int *wr_fd);

// Open a file on the given name, possibly closing a handle by that name
bool open_fd(const char *name, const char *path, int mode, int *fd);

// Close a named handle
bool close_fd(const char *name);

int fd_by_name(const char *name);

// Set metadata for a service.  Creates the service if it doesn't exist.
// Might fail if the metadata + env + argv are longer than the allowed buffer.
bool svc_set_meta(const char *name, const char *tsv_fields);

// Set args for a service.  Creates the service if it doesn't exist.
// Might fail if the metadata + env + argv are longer than the allowed buffer.
bool svc_set_argv(const char *name, const char *tsv_fields);

// Set env for a service.  Created the service if it doesn't exist.
// Might fail if the metadata + env + argv are longer than the allowed buffer.
bool svc_set_env(const char *name, const char *tsv_fields);

// Initialize service struct
void svc_new(service_t *svc, const char *name);

// update service state machine when requested to start
void svc_handle_start(service_t *svc, int wstat);

// update service state machine after PID is reaped
void svc_handle_reap(service_t *svc, int wstat);

// Deallocate service struct
void svc_free(service_t *svc);

service_t *svc_by_name(const char *name);
service_t *svc_by_pid(pid_t pid);

// Queue a message to the controller, possibly overflowing the output buffer
// and requiring a state reset event.
void ctl_queue_message(char *msg);

// Handle state transitions based on communication filehandles to the controller
void ctl_run(controller_t *ctl);

int main(int argc, char** argv) {
	wake_t wake;
	int wstat;
	pid_t pid;
	service_t *svc, *next;
	memset(&wake, 0, sizeof(wake));

	// Set up signal handlers and signal mask and signal self-pipe
	sig_setup();
	
	// parse arguments, updating the controller object, and possibly
	// allocating the FD and Service arrays.
	if (!parse_opts(argv))
		return 2;
	
	// Lock all memory into ram. init should never be "swapped out".
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
		perror("mlockall");
	
	// terminate is disabled when running as init, so this is an infinite loop
	// (except when debugging)
	while (!main_state.terminate) {
		// set our wait parameters so other methods can inject new wake reasons
		wake.now= gettime_us();
		wake.next= wake.now + 60*1000000; // wake at least every 60 seconds
		FD_ZERO(&wake.fd_read);
		FD_ZERO(&wake.fd_write);
		FD_ZERO(&wake.fd_err);
		
		// Always watch signal-pipe
		FD_SET(sig_wake_rd, &wake.fd_read);
		FD_SET(sig_wake_rd, &wake.fd_err);
		wake.max_fd= sig_wake_rd;
		
		// signals off
		sigset_t maskall, old;
		sigfillset(&maskall);
		if (!sigprocmask(SIG_SETMASK, &maskall, &old) == 0)
			perror("sigprocmask(all)");
	
		// report signals
		sig_send_notifications();
		
		// reap all zombies, possibly waking services
		while ((pid= waitpid(-1, &wstat, WNOHANG)) > 0)
			if ((svc= svc_by_pid(pid)))
				svc_handle_reaped(svc, wstat);
		
		// run controller state machine
		ctl_run(&controller, &wake);
		
		// run state machine of each service that is active.
		// services can remove themselves form the list, so need to track 'next'.
		for (cur= next= svc_active_list; next; cur= next) {
			next= cur->active_next;
			svc_run(cur);
		}
		
		// resume normal signal mask
		if (!sigprocmask(SIG_SETMASK, &old, NULL) == 0)
			perror("sigprocmask(reset)");

		// Wait until an event or the next time a state machine needs to run
		// (state machines edit wake.next)
		if (select(max_fd, &read_set, &write_set, &err_set, NULL) < 0) {
			// shouldn't ever fail, but if not EINTR, at least log it and prevent
			// looping too fast
			if (errno != EINTR) {
				perror("select");
				sleep(1);
			}
		}
	}
	
	return 0;
}

int64_t gettime_us() {
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
		t.tv_sec= time(NULL);
		t.tv_nsec= 0;
	}
	return ((int64_t) t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}

//---------------------------
// Service methods

bool svc_by_name_inorder(const service_t *a, const service_t *b) {
	return strcmp(a->buffer, b->buffer) <= 0;
}
int  svc_by_name_key_compare(const char *key, const service_t *svc) {
	return strcmp(*key, svc->buffer);
}

inline service_t *svc_by_name(const char *name) {
	RBTreeNode* node;
	// services can be lazy-initialized
	if (services
		&& (node= RBTree_Find( &service_by_name_index, name,
				(RBTree_compare_func*) svc_by_name_key_compare)))
		return (service_t*) node->Object;
	return NULL;
}

bool svc_by_pid_inorder(const service_t *a, const service_t *b) {
	return a->pid <= b->pid;
}
int  svc_by_pid_key_compare(pid_t *key, const service_t *svc) {
	return *key < svc->pid? -1 : *key > svc->pid? 1 : 0;
}

inline service_t *svc_by_pid(pid_t pid) {
	RBTreeNode* node;
	// services can be lazy-initialized
	if (services
		&& (node= RBTree_Find( &service_by_pid_index, &pid,
				(RBTree_compare_func*) svc_by_pid_key_compare)))
		return (service_t*) node->Object;
	return NULL;
}

void svc_handle_reaped(srvice_t *svc, int wstat) {
	svc->reaped= true;
	svc->wait_status= wstat;
	svc_set_active(svc, true);
}

void svc_set_active(service_t *svc, bool activate) {
	if (activate && !svc->active_prev_ptr) {
		// Insert node at head of doubly-linked list
		svc->active_prev_ptr= &svc_active_list;
		svc->active_next= svc_active_list;
		if (svc_active_list)
			svc_active_list->active_prev_ptr= &svc->active_next;
		svc_active_list= svc;
	}
	else if (!activate && svc->active_prev_ptr) {
		// remove node from doubly linked list
		if (svc->active_next)
			svc->active_next->active_prev_ptr= svc->active_prev_ptr;
		svc->active_prev_ptr= svc->active_next;
	}
}

void svc_run(service_t *svc, wake_t *wake) {
	pid_t pid;
	int exit;
	int sig;
	switch (svc->state) {
	case SVC_STATE_START_PENDING: svc_state_start_pending:
		// if not wake time yet,
		if (svc->start_time - wake->now > 0) {
			// set main-loop wake time if we're next
			if (svc->start_time - wake->next < 0)
				wake->next= svc->start_time;
			// ensure listed as active
			svc_set_active(svc, true);
			break;
		}
		// else we've reached the time to retry
		svc->state= SVC_STATE_START;
		svc_report_state(svc);
	case SVC_STATE_START: svc_state_start:
		pid= fork();
		if (pid > 0) {
			svc->pid= pid;
			svc->start_time= wake->now;
		} else if (pid == 0) {
			svc_do_exec(svc);
			// never returns
		} else {
			// else fork failed, and we need to wait 3 sec and try again
			svc->start_time= wake->now + FORK_RETRY_DELAY;
			svc->state= SVC_STATE_START_FAIL;
			svc_report_state(svc);
			goto svc_state_start_pending;
		}
		svc->state= SVC_STATE_UP;
		svc_report_state(svc);
	case SVC_STATE_UP:
		svc_set_active(svc, false);
		// waitpid in main loop will re-activate us and set state to REAPED
		break;
	case SVC_STATE_REAPED:
		svc->reap_time= wake->now;
		svc_report_state(svc);
		if (svc->auto_restart) {
			// if restarting too fast, delay til future
			if (svc->reap_time - svc->start_time < SERVICE_RESTART_DELAY) {
				svc->start_time= svc->reap_time + SERVICE_RESTART_DELAY;
				svc->state= SVC_STATE_START_PENDING;
				svc_report_state(svc);
				goto svc_state_start_pending;
			} else {
				svc->state= SVC_STATE_START;
				svc_report_state(svc);
				goto svc_state_start;
			}
		}
		svc->state= SVC_STATE_DOWN;
	case SVC_STATE_DOWN:
		svc_set_active(svc, false);
		break;
	// We can only arrive here as a result of a bug.  Catch it with asserts.
	case SVC_STATE_UNDEF:
		assert(svc->state != SVC_STATE_UNDEF);
	default:
		assert(0)
	}
}

void svc_report_meta(service_t *svc) {
	
}

void svc_report_state(service_t *svc, wake_t *wake) {
	switch (svc->state) {
	case SVC_STATE_START:
		ctl_queue_message("service	%s	state starting	0.000", svc->buffer);
		break;
	case SVC_STATE_START_PENDING:
		ctl_queue_message("service	%s	state starting	%.3f",
			svc->buffer,
			(svc->start_time - wake->now) / 1000000.0);
		break;
	case SVC_STATE_UP:
		ctl_queue_message("service	%s	state up	%.3f	pid	%d",
			svc->buffer,
			(wake->now - svc->start_time) / 1000000.0,
			(int) svc->pid);
		break;
	case SVC_STATE_REAPED:
	case SVC_STATE_DOWN:
		if (svc->pid == 0)
			ctl_queue_message("service	%s	state down", svc->buffer);
		else if (WIFEXITED(svc->wait_status))
			ctl_queue_message("service	%s	state down	%.3f	exit	%d	uptime %.3f	pid	%d",
				svc->buffer,
				(wake->now - svc->reap_time) / 1000000.0,
				WEXITSTATUS(svc->wait_status),
				(svc->reap_time - svc->start_time) / 1000000.0,
				(int) svc->pid);
		else
			ctl_queue_message("service	%s	state down	%.3f	signal	%s	uptime %.3f	pid	%d",
				svc->buffer,
				(wake->now - svc->reap_time) / 1000000.0,
				sig_name(WTERMSIG(svc->wait_status)),
				(svc->reap_time - svc->start_time) / 1000000.0,
				(int) svc->pid);
		break;
	default:
		ctl_queue_message("service	%s	INVALID!", svc->buffer);
	}
}

//---------------------------
// Controller methods

void ctl_run(controller_t *ctl, wake_t *wake) {
	switch (ctl->state) {
	}
	
	// If incoming fd, wake on data available
	// (this could also be the config file, initially)
	if (ctl->recv_fd != -1) {
		FD_SET(ctl->recv_fd, &wake->fd_read);
		FD_SET(ctl->recv_fd, &wake->fd_err);
		if (ctl->recv_fd > wake->max_fd)
			wake->max_fd= ctl->revc_fd;
	}
	// if anything was left un-written, wake on writable pipe
	if (ctl->send_fd != -1 && ctl->out_buf_len > 0) {
		FD_SET(ctl->send_fd, &wake->fd_write);
		FD_SET(ctl->send_fd, &wake->fd_err);
		if (ctl->send_fd > wake->max_fd)
			wake->max_fd= ctl->send_fd;
	}
}

//---------------------------
// FD Methods

bool fd_by_name_inorder(const fd_t *a, const fd_t *b) {
	return strcmp(a->name, b->name) <= 0;
}
int fd_by_name_key_compare(const char *str, const fd_t *b) {
	return strcmp(str, b->name);
}

int fd_by_name(const char *name) {
	if (fds
		&& (node= RBTree_Find( &fd_by_name_index, name,
			(RBTree_compare_func*) fd_by_name_key_compare )))
		return ((fd_t*) node->Object) - fds;
	return -1;
}

//---------------------------
// Note on signal handling:
//
// I'm aware that there are more elegant robust ways to write signal handling,
// but frankly, I'm sick of all the song and dance and configure tests, and I
// decided to just go the absolute minimal most portable route this time.

void sig_handler(int sig) {
	switch (sig) {
	case SIGINT: got_sigint= 1; break;
	case SIGHUP: got_sighup= 1; break;
	case SIGTERM: got_sigterm= 1; break;
	case SIGUSR1: got_sigusr1= 1; break;
	case SIGUSR2: got_sigusr2= 1; break;
	}
	write(sig_wake_wr, "", 1);
}

void sig_setup() {
	int pipe_fd[2];
	struct sigaction act, act_chld, act_ign;
	
	// Create pipe and set non-blocking
	if (pipe(pipe_fd)
		|| fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[1], F_SETFD, FD_CLOEXEC)
		|| fcntl(pipe_fd[0], F_SETFL, O_NONBLOCK)
		|| fcntl(pipe_fd[1], F_SETFL, O_NONBLOCK)
	) {
		perror("signal pipe setup");
		abort();
	}
	sig_wake_rd= pipe_fd[0];
	sig_wake_wr= pipe_fd[1];
	
	// set signal handlers
	memset(&act, 0, sizeof(act));
	act.sa_handler= (void(*)(int)) sig_handler;
	if (sigaction(SIGINT, &act, NULL)
		|| sigaction(SIGHUP,  &act, NULL)
		|| sigaction(SIGTERM, &act, NULL)
		|| sigaction(SIGUSR1, &act, NULL)
		|| sigaction(SIGUSR2, &act, NULL)
		|| (act.sa_handler= SIG_DFL, sigaction(SIGCHLD, &act, NULL))
		|| (act.sa_handler= SIG_IGN, sigaction(SIGPIPE, &act, NULL))
	) {
		perror("signal handler setup");
		abort();
	}
}

void sig_send_notifications() {
	char buf[32];
	
	// drain the signal pipe (pipe is nonblocking)
	while (read(sig_wake_rd, buf, sizeof(buf)) > 0);
	
	// report caught signals
	if (got_sigint) ctl_queue_message("signal INT");
	if (got_sighup) ctl_queue_message("signal HUP");
	if (got_sigterm) ctl_queue_message("signal TERM");
	if (got_sigusr1) ctl_queue_message("signal USR1");
	if (got_sigusr2) ctl_queue_message("signal USR2");
	
	// reset flags
	got_sigint= got_sighup= got_sigterm= got_sigusr1= got_sigusr2= 0;
}