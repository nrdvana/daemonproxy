#include "config.h"
#include "daemonproxy.h"
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

struct service_s {
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
};

// Define a sensible minimum for service size.
// Want at least struct size, plus room for name, small argv list, and short names of file descriptors
const int min_service_obj_size= sizeof(service_t) + NAME_LIMIT + 128;

void *svc_pool= NULL;
RBTree svc_by_name_index;
RBTree svc_by_pid_index;
service_t *svc_active_list= NULL; // linked list of active services
service_t *svc_free_list= NULL; // linked list re-using next_active and prev_active

void svc_notify_state(service_t *svc);
void svc_change_pid(service_t *svc, pid_t pid);
void svc_do_exec(service_t *svc);
void svc_set_active(service_t *svc, bool activate);
static void svc_reparse_meta(service_t *svc);

int  svc_by_name_compare(void *data, RBTreeNode *node) {
	strseg_t *name= (strseg_t*) data;
	service_t *obj= (service_t*) node->Object;
	return strseg_cmp(*name, (strseg_t){ obj->buffer, obj->name_len });
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

bool svc_check_name(strseg_t name) {
	const char *p, *lim;
	if (name.len >= NAME_LIMIT)
		return false;
	for (p= name.data, lim= p+name.len; p < lim; p++)
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

pid_t   svc_get_pid(service_t *svc) {
	return svc->pid;
}
int     svc_get_wstat(service_t *svc) {
	return svc->wait_status;
}
int64_t svc_get_up_ts(service_t *svc) {
	return svc->start_time;
}
int64_t svc_get_reap_ts(service_t *svc) {
	return svc->reap_time;
}

/** Set the string for the service's metadata
 * This string is concatenated with argv and fds in a single buffer.
 * This is slightly expensive, but typically happens only once per service.
 */
bool svc_set_meta(service_t *svc, strseg_t tsv_fields) {
	if (svc->name_len + 1 + tsv_fields.len + 1 + svc->argv_len + 1 + svc->fds_len + 1
		> svc->size - sizeof(service_t)
	)
		return false;
	// memmove the argv and fds strings to the new end of this string
	if (tsv_fields.len != svc->meta_len)
		memmove(svc->buffer + svc->name_len + 1 + tsv_fields.len + 1,
			svc->buffer + svc->name_len + 1 + svc->meta_len + 1,
			svc->argv_len + 1 + svc->fds_len + 1);
	memcpy(svc->buffer + svc->name_len + 1, tsv_fields.data, tsv_fields.len+1);
	svc->meta_len= tsv_fields.len;
	svc_reparse_meta(svc);
	// unless NDEBUG:
		svc_check(svc);
		assert(strncmp(svc_get_meta(svc), tsv_fields.data, tsv_fields.len) == 0);
	return true;
}

bool svc_apply_meta(service_t *svc, strseg_t name, strseg_t value) {
	// See if we have a metadata field of this name yet
	char *p, *p2, *v= NULL;
	int sizediff, buf_used;
	p= p2= svc->buffer + svc->name_len + 1;
	while (*p2) {
		while (*p2 && *p2 != '\t') p2++;
		if (0 == strncmp(name.data, p, name.len) && p[name.len] == '=') {
			v= p+name.len+1;
			break;
		}
		if (*p2) p= ++p2;
	}
	// find change in size of metadata
	sizediff= v? (value.len - (p2 - v)) : name.len + 1 + value.len + 1;
	buf_used= svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1 + svc->fds_len + 1;
	if (buf_used + sizediff > svc->size - sizeof(service_t))
		return false;
	// memmove the strings after the value
	if (sizediff)
		memmove(p2 + sizediff, p2, buf_used - (p2 - svc->buffer));
	// create "name=" if didn't exist
	if (!v) {
		if (p2 != svc->buffer+svc->name_len+1)
			*p2++ = '\t';
		memcpy(p2, name.data, name.len);
		p2[name.len]= '=';
		v= p2 + name.len + 1;
	}
	// overwrite value
	memcpy(v, value.data, value.len);
	svc->meta_len += sizediff;
	svc_reparse_meta(svc);
	// unless NDEBUG:
		svc_check(svc);
	return true;
}

/** Parse known meta flags.
 */
void svc_reparse_meta(service_t *svc) {
	const char *p, *p2;
	char *end;
	int i;
	
	// reset flags to defaults
	svc->auto_restart= false;
	
	p= p2= svc_get_meta(svc);
	while (*p2) {
		while (*p2 && *p2 != '\t') p2++;
		if (strncmp(p, "auto-restart=", 13) == 0) {
			i= strtol(p+13, &end, 10);
			if (end == p2 && i)
				svc->auto_restart= true;
		}
		if (*p2) p= ++p2;
	}
}

/** Set the string for the service's argument list
 * This string is concatenated with meta and fds in a single buffer.
 * This is slightly expensive, but typically happens only once per service.
 */
bool svc_set_argv(service_t *svc, strseg_t new_argv) {
	// Check whether new value will fit in buffer
	if (svc->name_len + 1 + svc->meta_len + 1 + new_argv.len + 1 + svc->fds_len + 1
		> svc->size - sizeof(service_t)
	)
		return false;
	// memmove the "fds" string to the new end of this string
	if (new_argv.len != svc->argv_len)
		memmove(svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + new_argv.len + 1,
			svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1,
			svc->fds_len + 1);
	memcpy(svc->buffer + svc->name_len + 1 + svc->meta_len + 1, new_argv.data, new_argv.len+1);
	svc->argv_len= new_argv.len;
	// unless NDEBUG:
		svc_check(svc);
		assert(strncmp(svc_get_argv(svc), new_argv.data, new_argv.len) == 0);
	return true;
}

/** Set the string for the service's file descriptor specification
 * This string is concatenated with meta and argv in a single buffer.
 * This is slightly expensive, but typically happens only once per service.
 */
bool svc_set_fds(service_t *svc, strseg_t new_fds) {
	// Check whether new value will fit in buffer
	if (svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1 + new_fds.len + 1
		> svc->size - sizeof(service_t)
	)
		return false;
	// TODO: parse tsv_fields for correct syntax
	
	// It's the last field, so no memmove needed
	memcpy(svc->buffer + svc->name_len + 1 + svc->meta_len + 1 + svc->argv_len + 1,
		new_fds.data, new_fds.len+1);
	svc->fds_len= new_fds.len;
	// unless NDEBUG:
		svc_check(svc);
		assert(strncmp(svc_get_fds(svc), new_fds.data, new_fds.len) == 0);
	return true;
}

bool svc_handle_start(service_t *svc, int64_t when) {
	if (svc->state != SVC_STATE_DOWN && svc->state != SVC_STATE_START_PENDING) {
		log_debug("Can't start service \"%s\": state is %d", svc_get_name(svc), svc->state);
		return false;
	}
	
	if (when - wake->now > 0) {
		log_debug("start service \"%s\" in %d seconds", svc_get_name(svc), (int)((when - wake->now) >> 32));
		svc->state= SVC_STATE_START_PENDING;
		svc->start_time= when;
	}
	else {
		log_debug("start service \"%s\" now", svc_get_name(svc), (int)((when - wake->now) >> 32));
		svc->state= SVC_STATE_START;
		svc->start_time= wake->now;
	}
	if (svc->start_time == 0) svc->start_time++; // 0 means undefined
	svc_change_pid(svc, 0);
	svc->reap_time= 0;
	svc->wait_status= -1;
	svc_set_active(svc, true);
	svc_notify_state(svc);
	return true;
}

/** Handle the case where a service's pid was reaped with wait().
 * This wakes up the service state machine, to possibly restart the daemon.
 * It is assumed that this is called by main() before iterating the active services.
 */
void svc_handle_reaped(service_t *svc, int wstat) {
	if (svc->state == SVC_STATE_UP) {
		log_trace("Setting service \"%s\" state to reaped", svc_get_name(svc));
		svc->wait_status= wstat;
		svc->state= SVC_STATE_REAPED;
		svc->reap_time= wake->now;
		svc_set_active(svc, true);
	}
	else log_trace("Service \"%s\" pid %d reaped, but service is not up", svc_get_name(svc), svc->pid);
}

/** Send a signal to a service iff it is running.
 */
bool svc_send_signal(service_t *svc, int signum, bool group) {
	if (!svc || svc->pid <= 0) return false;
	
	log_debug("Sending signal %d to service \"%s\" pid %d", signum, svc_get_name(svc), (int)svc->pid);
	return 0 == (group? killpg(svc->pid, signum) : kill(svc->pid, signum));
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
		svc->active_prev_ptr= NULL;
	}
	// unless NDEBUG:
		svc_check(svc);
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
	log_trace("service %s state = %d", svc_get_name(svc), svc->state);
	re_switch_state:
	switch (svc->state) {
	case SVC_STATE_START_PENDING:
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
	case SVC_STATE_START:
		pid= fork();
		if (pid > 0) {
			svc_change_pid(svc, pid);
			svc->start_time= wake->now;
			if (!svc->start_time) svc->start_time= 1;
		} else if (pid == 0) {
			svc_do_exec(svc);
			assert(0);
			// never returns
		} else {
			// else fork failed, and we need to wait 3 sec and try again
			svc_handle_start(svc, wake->now + FORK_RETRY_DELAY);
			svc_notify_state(svc);
			goto re_switch_state;
		}
		svc->state= SVC_STATE_UP;
		svc_notify_state(svc);
	case SVC_STATE_UP:
		svc_set_active(svc, false);
		// waitpid in main loop will re-activate us and set state to REAPED
		break;
	case SVC_STATE_REAPED:
		svc_notify_state(svc);
		svc->state= SVC_STATE_DOWN;
		if (svc->auto_restart) {
			// if restarting too fast, delay til future
			svc_handle_start(svc, 
				(svc->reap_time - svc->start_time < SERVICE_RESTART_INTERVAL)?
				wake->now + SERVICE_RESTART_INTERVAL : wake->now);
			svc_notify_state(svc);
		}
		goto re_switch_state;
	case SVC_STATE_DOWN:
		svc_set_active(svc, false);
		break;
	// We can only arrive here as a result of a bug.  Catch it with asserts.
	case SVC_STATE_UNDEF:
		assert(svc->state != SVC_STATE_UNDEF);
	default:
		assert(0);
	}
	// unless NDEBUG:
		svc_check(svc);
}

/** Perform the exec() to launch the service's daemon (or runscript)
 * This sets up FDs, and calls exec() with the argv for the service.
 */
void svc_do_exec(service_t *svc) {
	int fd_count, arg_count, i;
	int *fd_list;
	fd_t *fd;
	char **argv, *fd_spec, *arg_spec, *p, *p2;

	// clear signal mask
	sig_reset_for_exec();
	
	// Make mutable copies (which we don't need to free because we're calling exec)
	fd_spec= strdup(svc_get_fds(svc));
	arg_spec= strdup(svc_get_argv(svc));
	if (!fd_spec || !arg_spec) {
		log_error("out of memory");
		abort();
	}

	// count our file descriptors, to allocate buffer
	if (fd_spec[0]) {
		for (fd_count= 1, p= fd_spec; *p; p++)
			if (*p == '\t')
				fd_count++;
		fd_list= alloca(fd_count);
		// now iterate again to resolve them from name to number
		fd_count= 0;
		p= p2= fd_spec;
		while (1) {
			while (*p2 && *p2 != '\t') p2++;
			if (p == p2)
				log_warn("ignoring zero-length file descriptor name");
			else if (*p == '-' && p2 - p == 1)
				fd_list[fd_count++]= -1; // dash means "closed"
			else {
				fd= fd_by_name((strseg_t){ p, p2-p });
				if (!fd) {
					log_error("file descriptor \"%.*s\" does not exist", p2-p, p);
					abort();
				}
				fd_list[fd_count++]= fd_get_fdnum(fd);
			}
			if (!*p2) break;
			p= ++p2;
		}
	} else {
		// default file descriptors are to put '/dev/null' on stdin,stdout,stderr
		fd_count= 3;
		fd_list= alloca(fd_count);
		fd= fd_by_name(STRSEG("null"));
		fd_list[0]= fd_list[1]= fd_list[2]= (fd? fd_get_fdnum(fd) : -1);
	}
	
	// Now move them into correct places
	// But first, we need to make sure all the file descriptors we're about to copy
	//   are out of the way...
	for (i= 0; i < fd_count; i++) {
		// If file descriptor is less than the max destination, dup it to a higher number
		// We have no way to know which higher numbers are safe, so repeat until we get one.
		while (fd_list[i] >= 0 && fd_list[i] < fd_count) {
			fd_list[i]= dup(fd_list[i]);
			if (fd_list[i] < 0) {
				log_error("failed to dup file descriptor %d", fd_list[i]);
				abort();
			}
		}
	}
	// Now dup2 each into its correct slot, and close the rest
	for (i= 0; i < fd_count; i++) {
		if (fd_list[i] >= 0) {
			if (dup2(fd_list[i], i) < 0) {
				log_error("failed to dup file descriptor %d to %d", fd_list[i], i);
				abort();
			}
		}
		else close(i);
	}
	// close all fd we arent keeping
	while (i < FD_SETSIZE) close(i++);
	
	// convert argv into pointers
	// count, allocate, then populate
	for (arg_count= 1, p= arg_spec; *p; p++)
		if (*p == '\t')
			arg_count++;
	argv= alloca(arg_count+1);
	// then populate
	i= 0;
	for (argv[0]= p= arg_spec; *p; p++)
		if (*p == '\t') {
			*p= '\0';
			argv[++i]= p+1;
		}
	argv[++i]= NULL;
	
	p= NULL;
	execvpe(argv[0], argv, &p);
	log_error("exec(%s, ...) failed: %s", argv[0], strerror(errno));
	_exit(EXIT_INVALID_ENVIRONMENT);
}
	
void svc_notify_state(service_t *svc) {
	log_trace("service %s state = %d", svc_get_name(svc), svc->state);
	ctl_notify_svc_state(NULL, svc->buffer, svc->start_time, svc->reap_time, svc->wait_status, svc->pid);
}

service_t *svc_by_name(strseg_t name, bool create) {
	service_t *svc;
	
	RBTreeSearch s= RBTree_Find( &svc_by_name_index, &name );
	if (s.Relation == 0)
		return (service_t*) s.Nearest->Object;
	// if create requested, create a new service by this name
	// (if name is valid)
	if (create && svc_free_list && svc_check_name(name)) {
		svc= svc_free_list;
		svc_free_list= svc->next_free;
		svc->state= SVC_STATE_DOWN;
		svc->name_len= name.len;
		memcpy(svc->buffer, name.data, name.len);
		svc->meta_len= 0;
		svc->buffer[name.len+1]= '\0';
		svc->argv_len= 0;
		svc->buffer[name.len+2]= '\0';
		svc->fds_len= 0;
		svc->buffer[name.len+3]= '\0';
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
		RBTree_Add( &svc_by_name_index, &svc->name_index_node, &name );
		// unless NDEBUG:
			svc_check(svc);
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
		RBTree_Add( &svc_by_pid_index, &svc->pid_index_node, &svc->pid );
	}
	// unless NDEBUG:
		svc_check(svc);
}

service_t *svc_by_pid(pid_t pid) {
	RBTreeSearch s= RBTree_Find( &svc_by_pid_index, &pid );
	if (s.Relation == 0)
		return (service_t*) s.Nearest->Object;
	return NULL;
}

service_t * svc_iter_next(service_t *svc, strseg_t from_name) {
	RBTreeNode *node;
	log_trace("next service from %p or \"%.*s\"", svc, from_name.len, from_name.data);
	if (svc) {
		node= RBTreeNode_GetNext(&svc->name_index_node);
	} else {
		RBTreeSearch s= RBTree_Find( &svc_by_name_index, &from_name );
		log_trace("find(\"%.*s\"): { %d, %p }", from_name.len, from_name.data, s.Relation, s.Nearest);
		if (s.Nearest == NULL)
			node= NULL;
		else if (s.Relation < 0) // If key is less than returned node,
			node= s.Nearest;     // then we've got the "next node"
		else                    // else we got the "prev node" and need the next one
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
