#include "config.h"
#include "init-frame.h"
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
	int meta_count, arg_count, env_count, fd_count;
	RBTreeNode name_index_node;
	RBTreeNode pid_index_node;
	struct service_s **active_prev_ptr, *active_next;
	pid_t pid;
	bool auto_restart: 1;
	int wait_status;
	int64_t start_time;
	int64_t reap_time;
	char buffer[];
} service_t;

service_t *svc_active_list= NULL;
service_t *svc_free_list= NULL; // linked list re-using next_active and prev_active
int svc_active_count;

// Indexes and lists of services.
RBTreeNode service_by_name_index;
RBTreeNode service_by_pid_index;
service_t *service_free_list; 

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
		ctl_queue_message("service.state	%s	starting	0.000", svc->buffer);
		break;
	case SVC_STATE_START_PENDING:
		ctl_queue_message("service.state	%s	state starting	%.3f",
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

void svc_run_active(wake_t *wake) {
	service_t *cur, *next;
	// services can remove themselves form the list, so need to track 'next'.
	for (cur= next= svc_active_list; next; cur= next) {
		next= cur->active_next;
		svc_run(cur, wake);
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
		ctl_queue_message("service.state	%s	starting	0.000", svc->buffer);
		break;
	case SVC_STATE_START_PENDING:
		ctl_queue_message("service.state	%s	state starting	%.3f",
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

