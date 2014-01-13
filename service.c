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
	int name_len, meta_len, argv_len, fds_len;
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

// Define a sensible minimum for service size.
// Want at least struct size, plus room for name, small argv list, and short names of file descriptors
const int min_service_obj_size= sizeof(service_t) + NAME_MAX + 128;

void *svc_pool= NULL;
RBTree svc_by_name_index;
RBTree svc_by_pid_index;
service_t *svc_active_list= NULL; // linked list of active services
service_t *svc_free_list= NULL; // linked list re-using next_active and prev_active

void svc_change_pid(service_t *svc, pid_t pid);
void svc_do_exec(service_t *svc);
void svc_set_active(service_t *svc, bool activate);

int  svc_by_name_compare(void *key, RBTreeNode *node) {
	return strcmp((char*) key, ((service_t*) node->Object)->buffer);
}
int  svc_by_pid_compare(void *key, RBTreeNode *node) {
	pid_t a= * (pid_t*) key;
	pid_t b= ((service_t*) node->Object)->pid;
	return a < b? -1 : a > b? 1 : 0;
}

void svc_init(int service_count, int size_each) {
	service_t *svc;
	int i;
	svc_pool= malloc(service_count * size_each);
	if (!svc_pool)
		abort();
	memset(svc_pool, 0, service_count * size_each);
	svc_free_list= svc_pool;
	for (i=0; i < service_count; i++) {
		svc= (service_t*) (((char*)svc_pool) + i * size_each);
		svc->size= size_each;
		svc->next_free= (i+1 >= service_count)? NULL
			: (service_t*) (((char*)svc_pool) + (i+1) * size_each);
	}
	RBTree_Init( &svc_by_name_index, svc_by_name_compare );
	RBTree_Init( &svc_by_pid_index,  svc_by_pid_compare );
}

const char * svc_get_name(service_t *svc) {
	return svc->buffer;
}

bool svc_check_name(const char *name) {
	const char *p= name;
	for (; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
			return false;
	return true;
}

const char * svc_get_meta(service_t *svc) {
	return svc->buffer + svc->name_len + 1;
}

const char * svc_get_argv(service_t *svc) {
	return svc->buffer + svc->name_len + 1 + svc->meta_len + 1;
}

const char * svc_get_fds(service_t *svc) {
	return svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1;
}

/** Set the string for the service's metadata
 * This string is concatenated with argv and fds in a single buffer.
 * This is slightly expensive, but typically happens only once per service.
 */
bool svc_set_meta(service_t *svc, const char *tsv_fields) {
	int new_meta_len= strlen(tsv_fields);
	if (svc->name_len + 1 + new_meta_len + 1 + svc->argv_len + 1 + svc->fds_len + 1
		> svc->size - sizeof(service_t)
	)
		return false;
	// memmove the argv and fds strings to the new end of this string
	if (new_meta_len != svc->meta_len)
		memmove(svc->buffer + svc->name_len + 1 + new_meta_len + 1,
			svc->buffer + svc->name_len + 1 + svc->meta_len + 1,
			svc->argv_len + 1 + svc->fds_len + 1);
	memcpy(svc->buffer + svc->name_len + 1, tsv_fields, new_meta_len+1);
	svc->meta_len= new_meta_len;
	// TODO: parse metadata values for known arguments, like "auto-restart"
	// unless NDEBUG:
		svc_check(svc);
		assert(strcmp(svc_get_meta(svc), tsv_fields) == 0);
	return true;
}

/** Set the string for the service's argument list
 * This string is concatenated with meta and fds in a single buffer.
 * This is slightly expensive, but typically happens only once per service.
 */
bool svc_set_argv(service_t *svc, const char *tsv_fields) {
	int new_argv_len= strlen(tsv_fields);
	// Check whether new value will fit in buffer
	if (svc->name_len + 1 + svc->meta_len + 1 + new_argv_len + 1 + svc->fds_len + 1
		> svc->size - sizeof(service_t)
	)
		return false;
	// memmove the "fds" string to the new end of this string
	if (new_argv_len != svc->argv_len)
		memmove(svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + new_argv_len + 1,
			svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1,
			svc->fds_len + 1);
	memcpy(svc->buffer + svc->name_len + 1 + svc->meta_len + 1, tsv_fields, new_argv_len+1);
	svc->argv_len= new_argv_len;
	// unless NDEBUG:
		svc_check(svc);
		assert(strcmp(svc_get_argv(svc), tsv_fields) == 0);
	return true;
}

/** Set the string for the service's file descriptor specification
 * This string is concatenated with meta and argv in a single buffer.
 * This is slightly expensive, but typically happens only once per service.
 */
bool svc_set_fds(service_t *svc, const char *tsv_fields) {
	int new_fds_len= strlen(tsv_fields);
	// Check whether new value will fit in buffer
	if (svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1 + new_fds_len + 1
		> svc->size - sizeof(service_t)
	)
		return false;
	// TODO: parse tsv_fields for correct syntax
	
	// It's the last field, so no memmove needed
	memcpy(svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1,
		tsv_fields, new_fds_len+1);
	svc->fds_len= new_fds_len;
	// unless NDEBUG:
		svc_check(svc);
		assert(strcmp(svc_get_fds(svc), tsv_fields) == 0);
	return true;
}

/** Handle the case where a service's pid was reaped with wait().
 * This wakes up the service state machine, to possibly restart the daemon.
 */
void svc_handle_reaped(service_t *svc, int wstat) {
	svc->wait_status= wstat;
	if (svc->state == SVC_STATE_UP)
		svc->state= SVC_STATE_REAPED;
	svc_set_active(svc, true);
}

/** Activate or deactivate a service.
 * This simply inserts or removes the service from a linked list.
 * Each service in the "active" list get processed each time the main loop wakes up.
 */
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
	#ifdef UNIT_TESTING
	svc_check(svc);
	#endif
}

/** Run the state machine for each active service.
 * Services might set themselves back to inactive during this loop.
 */
void svc_run_active(wake_t *wake) {
	service_t *svc= svc_active_list;
	while (svc) {
		service_t *next= svc->active_next;
		svc_run(svc, wake);
		svc= next;
	}
}

/** Run the state machine for one service.
 */
void svc_run(service_t *svc, wake_t *wake) {
	pid_t pid;
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
		svc_notify_state(svc);
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
			svc_notify_state(svc);
			goto svc_state_start_pending;
		}
		svc->state= SVC_STATE_UP;
		if (!svc_notify_state(svc))
			ctl_notify_overflow();
	case SVC_STATE_UP:
		svc_set_active(svc, false);
		// waitpid in main loop will re-activate us and set state to REAPED
		break;
	case SVC_STATE_REAPED:
		svc->reap_time= wake->now;
		if (!svc_notify_state(svc))
			ctl_notify_overflow();
		if (svc->auto_restart) {
			// if restarting too fast, delay til future
			if (svc->reap_time - svc->start_time < SERVICE_RESTART_DELAY) {
				svc->start_time= svc->reap_time + SERVICE_RESTART_DELAY;
				svc->state= SVC_STATE_START_PENDING;
				svc->pid= 0;
				svc->reap_time= 0;
				if (!svc_notify_state(svc))
					ctl_notify_overflow();
				goto svc_state_start_pending;
			} else {
				svc->state= SVC_STATE_START;
				svc->pid= 0;
				svc->reap_time= 0;
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
	#ifdef UNIT_TESTING
	svc_check(svc);
	#endif
}

/** Perform the exec() to launch the service's daemon (or runscript)
 * This sets up FDs, and calls exec() with the argv for the service.
 */
void svc_do_exec(service_t *svc) {
	// TODO
}

bool svc_notify_state(service_t *svc) {
	return ctl_notify_svc_state(svc->buffer, svc->start_time, svc->reap_time, svc->pid, svc->wait_status);
}

service_t *svc_by_name(const char *name, bool create) {
	service_t *svc;
	int n;
	
	RBTreeSearch s= RBTree_Find( &svc_by_name_index, name );
	if (s.Relation == 0)
		return (service_t*) s.Nearest->Object;
	// if create requested, create a new service by this name
	// (if name is valid)
	n= strlen(name);
	if (create && svc_free_list && n > 0 && n + 4 < svc_free_list->size - sizeof(service_t)) {
		svc= svc_free_list;
		svc_free_list= svc->next_free;
		svc->state= SVC_STATE_DOWN;
		svc->name_len= n;
		memcpy(svc->buffer, name, n+1);
		svc->meta_len= 0;
		svc->buffer[n+1]= '\0';
		svc->argv_len= 0;
		svc->buffer[n+2]= '\0';
		svc->fds_len= 0;
		svc->buffer[n+3]= '\0';
		//svc->env_count= 0;
		svc->active_prev_ptr= NULL;
		svc->active_next= NULL;
		svc->pid= 0;
		svc->auto_restart= 0;
		svc->start_time= 0;
		svc->reap_time= 0;
		RBTreeNode_Init( &svc->name_index_node );
		svc->name_index_node.Object= svc;
		RBTreeNode_Init( &svc->pid_index_node );
		RBTree_Add( &svc_by_name_index, &svc->name_index_node, svc->buffer);
		#ifdef UNIT_TESTING
		svc_check(svc);
		#endif
		return svc;
	}
	return NULL;
}

void svc_change_pid(service_t *svc, pid_t pid) {
	if (svc->pid)
		RBTreeNode_Prune( &svc->pid_index_node );
	svc->pid= pid;
	if (svc->pid) {
		RBTreeNode_Init( &svc->pid_index_node );
		svc->pid_index_node.Object= svc;
		RBTree_Add( &svc_by_pid_index, &svc->pid_index_node, &svc->pid);
	}
	#ifdef UNIT_TESTING
	svc_check(svc);
	#endif
}

service_t *svc_by_pid(pid_t pid) {
	RBTreeSearch s= RBTree_Find( &svc_by_pid_index, &pid );
	if (s.Relation == 0)
		return (service_t*) s.Nearest->Object;
	return NULL;
}

service_t * svc_iter_next(service_t *svc, const char *from_name) {
	RBTreeNode *node;
	log_trace("next service from %p or \"%s\"", svc, from_name? from_name : "");
	if (svc) {
		node= RBTreeNode_GetNext(&svc->name_index_node);
	} else {
		RBTreeSearch s= RBTree_Find( &svc_by_name_index, from_name );
		log_trace("find(\"%s\"): { %d, %p }", from_name, s.Relation, s.Nearest);
		if (s.Nearest == NULL)
			node= NULL;
		else if (s.Relation <= 0)
			node= s.Nearest;
		else
			node= RBTreeNode_GetNext(s.Nearest);
	}
	return node? (service_t *) node->Object : NULL;
}

void svc_delete(service_t *svc) {
	svc_set_active(svc, false);
	if (svc->pid > 0) {
		kill(svc->pid, SIGTERM);
		RBTreeNode_Prune( &svc->pid_index_node );
	}
	RBTreeNode_Prune( &svc->name_index_node );
	svc->next_free= svc_free_list;
	svc_free_list= svc;
}

#ifndef NDEBUG
void svc_check(service_t *svc) {
	int buf_len;
	assert(svc != NULL);
	assert(svc->size > min_service_obj_size);
	buf_len= svc->size - sizeof(service_t);
	assert(svc->name_len >= 0 && svc->name_len < buf_len);
	assert(svc->meta_len >= 0 && svc->meta_len < buf_len);
	assert(svc->argv_len >= 0 && svc->argv_len < buf_len);
	assert(svc->fds_len  >= 0 && svc->fds_len  < buf_len);
	assert(svc->name_len + svc->meta_len + svc->argv_len + svc->fds_len +4 <= buf_len);
	assert(svc->name_index_node.Color == RBTreeNode_Black || svc->name_index_node.Color == RBTreeNode_Red);
	if (svc->pid)
		assert(svc->pid_index_node.Color == RBTreeNode_Black || svc->pid_index_node.Color == RBTreeNode_Red);
	else
		assert(svc->pid_index_node.Color == RBTreeNode_Unassigned);
	assert(svc->buffer[svc->name_len] == 0);
	assert(svc->buffer[svc->name_len + 1 + svc->meta_len] == 0);
	assert(svc->buffer[svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len] == 0);
	assert(svc->buffer[svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1 + svc->fds_len] == 0);
}
#endif
