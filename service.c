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
	int size, state;
	int meta_count, arg_count, env_count, fd_count;
	RBTreeNode name_index_node;
	RBTreeNode pid_index_node;
	struct service_s **active_prev_ptr;
	union {
		struct service_s *active_next;
		struct service_s *next_free;
	};
	pid_t pid;
	bool auto_restart: 1;
	int wait_status;
	int64_t start_time;
	int64_t reap_time;
	char buffer[];
} service_t;

service_t *svc_pool= NULL;
RBTree svc_by_name_index;
RBTree svc_by_pid_index;
service_t *svc_active_list= NULL; // linked list of active services
service_t *svc_free_list= NULL; // linked list re-using next_active and prev_active

void svc_change_pid(service_t *svc, pid_t pid);

int  svc_by_name_key_compare(void *key, RBTreeNode *node) {
	return strcmp((char*) key, ((service_t*) node->Object)->buffer);
}
int  svc_by_pid_key_compare(void *key, RBTreeNode *node) {
	pid_t a= * (pid_t*) key;
	pid_t b= ((service_t*) node->Object)->pid;
	return a < b? -1 : a > b? 1 : 0;
}

void svc_build_pool(void *buffer, int service_count, int size_each) {
	int i;
	svc_pool= (service_t*) buffer;
	memset(buffer, 0, service_count * size_each);
	svc_free_list= svc_pool;
	for (i=0; i < service_count; i++) {
		svc_pool[i].size= size_each;
		svc_pool[i].next_free= svc_pool+i+1;
	}
	svc_pool[service_count-1].next_free= NULL;
	RBTree_Init( &svc_by_name_index,  );
	RBTree_Init( &svc_by_pid_index );
}

const char * svc_get_name(service_t *svc) {
	return svc->buffer;
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
		*svc->active_prev_ptr= svc->active_next;
	}
}

void svc_handle_reaped(service_t *svc, int wstat) {
	svc->wait_status= wstat;
	if (svc->state == SVC_STATE_UP)
		svc->state= SVC_STATE_REAPED;
	svc_set_active(svc, true);
}

void svc_report_state(service_t *svc, wake_t *wake);

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
		svc_report_state(svc, wake);
	case SVC_STATE_START: svc_state_start:
		pid= fork();
		if (pid > 0) {
			svc_change_pid(svc, pid);
			svc->start_time= wake->now;
		} else if (pid == 0) {
			svc_do_exec(svc);
			// never returns
		} else {
			// else fork failed, and we need to wait 3 sec and try again
			svc->start_time= wake->now + FORK_RETRY_DELAY;
			svc->state= SVC_STATE_START_PENDING;
			svc_report_state(svc, wake);
			goto svc_state_start_pending;
		}
		svc->state= SVC_STATE_UP;
		svc_report_state(svc, wake);
	case SVC_STATE_UP:
		svc_set_active(svc, false);
		// waitpid in main loop will re-activate us and set state to REAPED
		break;
	case SVC_STATE_REAPED:
		svc->reap_time= wake->now;
		svc_report_state(svc, wake);
		if (svc->auto_restart) {
			// if restarting too fast, delay til future
			if (svc->reap_time - svc->start_time < SERVICE_RESTART_DELAY) {
				svc->start_time= svc->reap_time + SERVICE_RESTART_DELAY;
				svc->state= SVC_STATE_START_PENDING;
				svc_report_state(svc, wake);
				goto svc_state_start_pending;
			} else {
				svc->state= SVC_STATE_START;
				svc_report_state(svc, wake);
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
		assert(0);
	}
}

void svc_report_meta(service_t *svc) {
	
}

#define CASE_SIG(sig) case sig: return #sig;
const char* sig_name(int sig_num) {
	switch (sig_num) {
	CASE_SIG(SIGHUP)
	CASE_SIG(SIGINT)
	CASE_SIG(SIGQUIT)
	CASE_SIG(SIGILL)
	CASE_SIG(SIGTRAP)
	CASE_SIG(SIGABRT)
	CASE_SIG(SIGBUS)
	CASE_SIG(SIGFPE)
	CASE_SIG(SIGKILL)
	CASE_SIG(SIGUSR1)
	CASE_SIG(SIGUSR2)
	CASE_SIG(SIGSEGV)
	CASE_SIG(SIGPIPE)
	CASE_SIG(SIGALRM)
	CASE_SIG(SIGTERM)
	default: return "unknown";
	}
}

void svc_report_state(service_t *svc, wake_t *wake) {
	switch (svc->state) {
	case SVC_STATE_START:
		ctl_queue_message("service.state	%s	starting	0.000", svc->buffer);
		break;
	case SVC_STATE_START_PENDING:
		ctl_queue_message("service.state	%s	starting	%.3f",
			svc->buffer,
			(wake->now- svc->start_time) / 1000000.0);
		break;
	case SVC_STATE_UP:
		ctl_queue_message("service.state	%s	up	%.3f	pid	%d",
			svc->buffer,
			(wake->now - svc->start_time) / 1000000.0,
			(int) svc->pid);
		break;
	case SVC_STATE_REAPED:
	case SVC_STATE_DOWN:
		if (svc->pid == 0)
			ctl_queue_message("service.state	%s	down", svc->buffer);
		else if (WIFEXITED(svc->wait_status))
			ctl_queue_message("service.state	%s	down	%.3f	exit	%d	uptime %.3f	pid	%d",
				svc->buffer,
				(wake->now - svc->reap_time) / 1000000.0,
				WEXITSTATUS(svc->wait_status),
				(svc->reap_time - svc->start_time) / 1000000.0,
				(int) svc->pid);
		else
			ctl_queue_message("service.state	%s	down	%.3f	signal	%d=%s	uptime %.3f	pid	%d",
				svc->buffer,
				(wake->now - svc->reap_time) / 1000000.0,
				WTERMSIG(svc->wait_status), sig_name(WTERMSIG(svc->wait_status)),
				(svc->reap_time - svc->start_time) / 1000000.0,
				(int) svc->pid);
		break;
	default:
		ctl_queue_message("service.state	%s	INVALID!", svc->buffer);
	}
}

service_t *svc_by_name(const char *name, bool create) {
	RBTreeNode* node;
	service_t *svc;
	int n;
	// services can be lazy-initialized
	if (!svc_pool) return NULL;
	
	if ((node= RBTree_Find( &svc_by_name_index, name, (RBTree_compare_func*) svc_by_name_key_compare)))
		return (service_t*) node->Object;
	// if create requested, create a new service by this name
	// (if name is valid)
	n= strlen(name);
	if (create && svc_free_list && n > 0 && n < svc_free_list->size - sizeof(service_t)) {
		svc= svc_free_list;
		svc_free_list= svc->next_free;
		memcpy(svc->buffer, name, n+1);
		svc->state= SVC_STATE_DOWN;
		svc->meta_count= 0;
		svc->arg_count= 0;
		svc->env_count= 0;
		svc->fd_count= 0;
		svc->active_prev_ptr= NULL;
		svc->active_next= NULL;
		svc->pid= 0;
		svc->auto_restart= 0;
		svc->start_time= 0;
		svc->reap_time= 0;
		RBTreeNode_Init( &svc->name_index_node );
		svc->name_index_node.Object= svc;
		RBTree_Add( &svc_by_name_index, &svc->name_index_node,
			(RBTree_inorder_func*) svc_by_name_inorder);
	}
	return NULL;
}

void svc_change_pid(service_t *svc, pid_t pid) {
	if (svc->pid)
		RBTree_Prune( &svc->pid_index_node );
	svc->pid= pid;
	if (svc->pid) {
		RBTreeNode_Init( &svc->pid_index_node );
		svc->pid_index_node.Object= svc;
		RBTree_Add( &svc_by_pid_index, &svc->pid_index_node,
			(RBTree_inorder_func*) svc_by_pid_inorder);
	}
}

service_t *svc_by_pid(pid_t pid) {
	RBTreeNode* node;
	// services can be lazy-initialized
	if (!svc_pool) return NULL;
	if ((node= RBTree_Find( &svc_by_pid_index, &pid, (RBTree_compare_func*) svc_by_pid_key_compare)))
		return (service_t*) node->Object;
	return NULL;
}

service_t * svc_iter_next(service_t *svc, const char *from_name) {
	RBTreeNode *node;
	if (svc) {
		node= RBTreeNode_GetNext(&svc->name_index_node);
	} else {
		RBTreeSearch s= RBTree_Find( &svc_by_name_index, from_name );
		if (s.Nearest == NULL)
			return NULL;
		else if (s.Relation > 0)
			return s.Relation;
		else
			return RBTreeNode_GetNext(s.Nearest);
	}
}

void svc_delete(service_t *svc) {
	svc_set_active(svc, false);
	if (svc->pid > 0) {
		kill(svc->pid, SIGTERM);
		RBTree_Prune( &svc->pid_index_node );
	}
	RBTree_Prune( &svc->name_index_node );
	svc->next_free= svc_free_list;
	svc_free_list= svc;
}