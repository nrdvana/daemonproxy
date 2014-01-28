#include "config.h"
#include "daemonproxy.h"
#include "Contained_RBTree.h"

// Describes a named file handle

#define FD_TYPE_UNDEF  0
#define FD_TYPE_FILE   1
#define FD_TYPE_PIPE_R 2
#define FD_TYPE_PIPE_W 3
#define FD_TYPE_INTERNAL 4

struct fd_s {
	int size;
	unsigned int
		type: 31,
		is_const: 1;
	RBTreeNode name_index_node;
	int fd;
	union {
		char *path;
		struct fd_s *pipe_peer;
		struct fd_s *next_free;
	};
	char buffer[];
};

// Define a sensible minimum for service size.
// Want at least struct size, plus room for name of fd and short filesystem path
const int min_fd_obj_size= sizeof(fd_t) + NAME_MAX * 2;

fd_t **fd_list= NULL;
int fd_list_count= 0, fd_list_limit= 0;
RBTree fd_by_name_index;
void *fd_obj_pool= NULL;
int fd_obj_pool_size_each= 0;

void add_fd_by_name(fd_t *fd);
void create_missing_dirs(char *path);

int fd_by_name_compare(void *data, RBTreeNode *node) {
	strseg_t *name= (strseg_t*) data;
	return strncmp(name->data, ((fd_t *) node->Object)->buffer, name->len);
}

void fd_init() {
	RBTree_Init( &fd_by_name_index, fd_by_name_compare );
}

bool fd_preallocate(int count, int size_each) {
	assert(fd_list == NULL);
	assert(fd_obj_pool == NULL);
	if (!fd_list_resize(count))
		return false;
	
	if (!(fd_obj_pool= malloc(count * size_each)))
		return false;
	fd_obj_pool_size_each= size_each;
	for (i= 0; i < fd_count; i++)
		fd_list[i]= ((char*) fd_pool) + size_each * i;
	return true;
}

bool fd_list_resize(int new_limit) {
	fd_t **new_list;
	assert(new_limit >= fd_list_count);
	new_list= realloc(fd_list, new_limit * sizeof(fd_t*));
	if (!new_list)
		return false;
	fd_list= new_list;
	fd_list_limit= new_limit;
}

fd_t * fd_new(int size, strseg_t name) {
	fd_t *obj;
	assert(size >= sizeof(fd_t) + name.len + 1);
	// enlarge the container if needed (and not using a pool)
	if (fd_list_count >= fd_list_limit) {
		if (fd_obj_pool || !fd_list_resize(fd_list_limit * 2))
			return NULL;
	// allocate space (unless using a pool)
	if (fd_obj_pool) {
		size= fd_obj_pool_size_each;
		if (size < sizeof(fd_t) + name.len + 1)
			return NULL;
		obj= fd_list[fd_list_count++];
	}
	else {
		if (!(obj= (fd_t*) malloc(size)))
			return false;
		fd_list[fd_list_count++]= obj;
	}
	memset(obj, 0, size);
	obj->size= size;
	obj->type= FD_TYPE_UNDEF;
	obj->fd= -1;
	RBTreeNode_Init( &obj->name_index_node );
	obj->name_index_node.Object= obj;
	memcpy(obj->buffer, name.data, name.len);
	obj->buffer[name.len]= '\0';

	RBTree_Add( &fd_by_name_index, &obj->name_index_node, &name );

	return obj;
}

// Close a named handle
void fd_delete(fd_t *fd) {
	// disassociate from other end of pipe, if its a pipe.
	if (fd->type == FD_TYPE_PIPE_R || fd->type == FD_TYPE_PIPE_W) {
		if (fd->pipe_peer)
			fd->pipe_peer->pipe_peer= NULL;
	}
	// close descriptor.
	if (fd->fd >= 0) {
		int result= close(fd->fd);
		log_trace("close(%d) => %d", fd->fd, result);
	}
	// Remove name from index
	RBTreeNode_Prune( &fd->name_index_node );
	// remove the pointer from fd_list (or swap, for obj pool)
	for (i= 0; i < fd_list_count; i++) {
		if (fd_list[i] == fd) {
			fd_list[i]= fd_list[--fd_list_count];
			// free the memory (unless object pool)
			if (fd_obj_pool)
				fd_list[fd_list_count]= fd;
			else {
				fd_list[fd_list_count]= NULL;
				free(fd);
			}
			break;
		}
	}
}

const char* fd_get_name(fd_t *fd) {
	return fd->buffer;
}

int fd_get_fdnum(fd_t *fd) {
	return fd->fd;
}

const char* fd_get_file_path(fd_t *fd) {
	return fd->type == FD_TYPE_FILE? fd->path : NULL;
}

const char* fd_get_pipe_read_end(fd_t *fd) {
	return fd->type == FD_TYPE_PIPE_W && fd->pipe_peer? fd->pipe_peer->buffer : NULL;
}

const char* fd_get_pipe_write_end(fd_t *fd) {
	return fd->type == FD_TYPE_PIPE_R && fd->pipe_peer? fd->pipe_peer->buffer : NULL;
}

bool fd_notify_state(fd_t *fd) {
	switch (fd->type) {
	case FD_TYPE_INTERNAL:
	case FD_TYPE_FILE: return ctl_notify_fd_state(NULL, fd->buffer, fd->path, NULL, NULL);
	case FD_TYPE_PIPE_R: return ctl_notify_fd_state(NULL, fd->buffer, NULL, NULL, fd->pipe_peer? fd->pipe_peer->buffer : "(closed)");
	case FD_TYPE_PIPE_W: return ctl_notify_fd_state(NULL, fd->buffer, NULL, fd->pipe_peer? fd->pipe_peer->buffer : "(closed)", NULL);
	default: return ctl_notify_error(NULL, "File descriptor has invalid state");
	}
}

// Open a pipe from one named FD to another
// returns a ref to the read end, which holds a ref to the write end.
fd_t * fd_new_pipe(int fd1, strseg_t name1, int fd2, strseg_t name2) {
	int pair[2]= { -1, -1 };
	fd_t *fd1, *fd2;
	if (!(fd1= fd_by_name(name1))) fd1= fd_new(sizeof(fd_t) + name1.len + 1, name1);
	if (!(fd2= fd_by_name(name2))) fd2= fd_new(sizeof(fd_t) + name2.len + 1, name2);
	// If failed to allocate/find either of them, give up
	// also fail if either of them is a constant
	if (!fd1 || !fd2 || fd1->is_const || fd2->is_const)
		goto fail_cleanup;
	
	// If fd1 is being overwritten, close it
	if (fd1->type == FD_TYPE_UNDEF && fd1->fd >= 0) {
		close(fd1->fd);
		log_trace("close(%d)", fd1->fd);
	}
	// same for fd2
	if (fd2->type == FD_TYPE_UNDEF && fd2->fd >= 0) {
		close(fd2->fd);
		log_trace("close(%d)", fd2->fd);
	}
	
	fd1->type= FD_TYPE_PIPE_R;
	fd1->fd= pair[0];
	fd1->pipe_peer= fd2;

	fd2->type= FD_TYPE_PIPE_W;
	fd2->fd= pair[1];
	fd2->pipe_peer= fd1;
	
	ctl_notify_fd_state(NULL, fd1);
	ctl_notify_fd_state(NULL, fd2);
	return fd1;
	
	fail_cleanup:
	if (fd1 && fd1->type == FD_TYPE_UNDEF)
		fd_delete(fd1);
	if (fd2 && fd2->type == FD_TYPE_UNDEF)
		fd_delete(fd2);
	return NULL;
}

// Open a file on the given name, possibly closing a handle by that name
fd_t * fd_new_file(int fd, strseg_t name, int open_mode, int flags, strseg_t path) {
	fd_t *fd_obj;
	
	if (!(fd_obj= fd_by_name(name)))
		fd_obj= fd_new(sizeof(fd_t) + name.len + 1 + opts.len + 1 + path.len + 1);
	if (!fd_obj || fd_obj->is_const)
		return NULL;
	
	// Now, try to perform the open
	
	
	// Overwrite (and possibly setup) the fd_t object
	if (fd_obj->type != FD_TYPE_UNDEF && fd_obj->fd >= 0) {
		int result= close(fd_obj->fd);
		log_trace("close(%d) => %d", fd_obj->fd, result);
	}
	
	fd_obj->fd= fd;
	fd_obj->type= FD_TYPE_FILE;
	
	// copy as much of path into the buffer as we can.
	fd_obj->path= fd_obj->buffer + strlen(name) + 1;
	buf_free= fd_obj->size - (fd_obj->path - (char*) fd_obj);
	n= strlen(path);
	if (n < buf_free)
		memcpy(fd_obj->path, path, n+1);
	// else truncate with "..."
	else if (3 < buf_free) {
		n= buf_free - 4;
		memcpy(fd_obj->path, path, n);
		memcpy(fd_obj->path + n, "...", 4);
	}
	// unless we don't even have 4 chars to spare, in which case we make it an empty string
	else fd_obj->path--;
	
	fd_notify_state(fd_obj);
	return fd_obj;
	
	fail_cleanup:
	if (fd_obj && fd_obj->type == FD_TYPE_UNDEF)
		fd_delete(fd_obj);
	return NULL;
}

fd_t *fd_assign(const char *name, int fd, bool is_const, const char *description) {
	int n, buf_free;
	fd_t *fd1= fd_by_name((strseg_t){ name, strlen(name) }, true);
	if (fd1->type != FD_TYPE_UNDEF && fd1->fd >= 0)
		close(fd1->fd);
	fd1->type= FD_TYPE_INTERNAL;
	fd1->fd= fd;
	fd1->is_const= is_const;
	// copy as much of path into the buffer as we can.
	fd1->path= fd1->buffer + strlen(name) + 1;
	buf_free= fd1->size - (fd1->path - (char*) fd1);
	n= strlen(description);
	if (n < buf_free)
		memcpy(fd1->path, description, n+1);
	// else truncate with "..."
	else if (3 < buf_free) {
		n= buf_free - 4;
		memcpy(fd1->path, description, n);
		memcpy(fd1->path + n, "...", 4);
	}
	// unless we don't even have 4 chars to spare, in which case we make it an empty string
	else fd1->path--;
	return fd1;
}

void create_missing_dirs(char *path) {
	char *end;
	for (end= strchr(path, '/'); end; end= strchr(end+1, '/')) {
		*end= '\0';
		mkdir(path, 0700); // would probably take longer to stat than to just let mkdir fail
		*end= '/';
	}
}

fd_t * fd_by_name(strseg_t name) {
	assert(name.len < NAME_LIMIT);
	RBTreeSearch s= RBTree_Find( &fd_by_name_index, &name );
	if (s.Relation == 0)
		return (fd_t*) s.Nearest->Object;
	return NULL;
}

fd_t * fd_iter_next(fd_t *current, const char *from_name) {
	RBTreeNode *node;
	if (current) {
		node= RBTreeNode_GetNext(&current->name_index_node);
	} else {
		RBTreeSearch s= RBTree_Find( &fd_by_name_index, from_name );
		if (s.Nearest == NULL)
			node= NULL;
		else if (s.Relation > 0)
			node= s.Nearest;
		else
			node= RBTreeNode_GetNext(s.Nearest);
	}
	return node? (fd_t *) node->Object : NULL;
}
