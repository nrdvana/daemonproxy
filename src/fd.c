#include "config.h"
#include "daemonproxy.h"
#include "Contained_RBTree.h"

// Describes a named file handle

#define FD_TYPE_UNDEF  0
#define FD_TYPE_FILE   1
#define FD_TYPE_PIPE_R 2
#define FD_TYPE_PIPE_W 3
#define FD_TYPE_SPECIAL 4

struct fd_s {
	int size;
	fd_flags_t flags;
	int fd;
	RBTreeNode name_index_node;
	union attr_union_u {
		struct file_attr_s {
			const char *path;
		} file;
		struct pipe_attr_s {
			struct fd_s *peer;
		} pipe;
	} attr;
	char buffer[];
};

fd_t **fd_list= NULL;
int fd_list_count= 0, fd_list_limit= 0;
RBTree fd_by_name_index;
void *fd_obj_pool= NULL;
int fd_obj_pool_size_each= 0;

bool fd_list_resize(int new_limit);
void add_fd_by_name(fd_t *fd);
void create_missing_dirs(char *path);
static const char * append_elipses(char *buffer, int bufsize, strseg_t source);

int fd_by_name_compare(void *data, RBTreeNode *node) {
	strseg_t *name= (strseg_t*) data;
	return strncmp(name->data, ((fd_t *) node->Object)->buffer, name->len);
}

void fd_init() {
	RBTree_Init( &fd_by_name_index, fd_by_name_compare );
}

bool fd_preallocate(int count, int size_each) {
	int i;
	assert(fd_list == NULL);
	assert(fd_obj_pool == NULL);
	
	if (!fd_list_resize(count))
		return false;
	
	if (!(fd_obj_pool= malloc(count * size_each)))
		return false;
	fd_obj_pool_size_each= size_each;
	for (i= 0; i < count; i++)
		fd_list[i]= (fd_t*) (((char*) fd_obj_pool) + size_each * i);
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
	return true;
}

fd_t * fd_new(int size, strseg_t name) {
	fd_t *obj;
	assert(size >= sizeof(fd_t) + name.len + 1);
	// enlarge the container if needed (and not using a pool)
	if (fd_list_count >= fd_list_limit)
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
	int i;
	// disassociate from other end of pipe, if its a pipe.
	if (fd->flags.pipe) {
		if (fd->attr.pipe.peer)
			fd->attr.pipe.peer->attr.pipe.peer= NULL;
	}
	// close descriptor.
	if (fd->fd >= 0) {
		int result= close(fd->fd);
		log_trace("close(%d) => %d", fd->fd, result);
	}
	// Remove name from index
	RBTreeNode_Prune( &fd->name_index_node );
	// remove the pointer from fd_list and free the mem (or swap within list, for obj pool)
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

fd_flags_t fd_get_flags(fd_t *fd) {
	return fd->flags;
}

const char* fd_get_file_path(fd_t *fd) {
	return !fd->flags.pipe? fd->attr.file.path : NULL;
}

fd_t * fd_get_pipe_peer(fd_t *fd) {
	return fd->flags.pipe? fd->attr.pipe.peer : NULL;
}

// Open a pipe from one named FD to another
// returns a ref to the read end, which holds a ref to the write end.
fd_t * fd_new_pipe(strseg_t name1, int num1, strseg_t name2, int num2) {
	// Find any existing FD object by these names
	fd_t *f1, *old1= fd_by_name(name1);
	fd_t *f2, *old2= fd_by_name(name2);
	
	// Fail if either name exists as a constant
	if ((old1 && old1->flags.is_const) || (old2 && old2->flags.is_const))
		return NULL;
	
	// Allocate new FD objects
	f1= fd_new(sizeof(fd_t) + name1.len + 1, name1);
	if (!f1) return NULL;
	
	f2= fd_new(sizeof(fd_t) + name2.len + 1, name2);
	if (!f2) {
		fd_delete(f1);
		return NULL;
	}
	
	// It worked, so delete the old ones, if any
	if (old1) fd_delete(old1);
	if (old2) fd_delete(old2);

	f1->flags.pipe= true;
	f1->flags.read= true;
	f1->fd= num1;
	f1->attr.pipe.peer= f2;

	f2->flags.pipe= true;
	f2->flags.write= true;
	f2->fd= num2;
	f2->attr.pipe.peer= f1;
	
	ctl_notify_fd_state(NULL, f1);
	ctl_notify_fd_state(NULL, f2);
	return f1;
}

// Open a file on the given name, possibly closing a handle by that name
fd_t * fd_new_file(strseg_t name, int fdnum, fd_flags_t flags, strseg_t path) {
	int buf_free;
	fd_t *f, *old= fd_by_name(name);
	
	// fail if name is a constant
	if (old && old->flags.is_const)
		return NULL;
	
	// Allocate new obj
	f= fd_new(sizeof(fd_t) + name.len + 1 + path.len + 1, name);
	if (!f) return NULL;
	
	// it worked, so delete the old one, if any
	if (old) fd_delete(old);
	
	f->flags= flags;
	f->fd= fdnum;
	f->flags= flags;
	// copy as much of path into the buffer as we can.
	buf_free= f->size - sizeof(fd_t) - name.len - 1;
	f->attr.file.path= append_elipses(f->buffer + name.len + 1, buf_free, path);
	
	ctl_notify_fd_state(NULL, f);
	return f;
}

const char* append_elipses(char *buffer, int bufsize, strseg_t source) {
	if (source.len < bufsize) {
		memcpy(buffer, source.data, source.len);
		buffer[source.len]= '\0';
		return buffer;
	}
	// else truncate with "..."
	else if (bufsize >= 4) {
		int n= bufsize - 4;
		memcpy(buffer, source.data, n);
		memcpy(buffer + n, "...", 4);
		return buffer;
	}
	else return "...";
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

fd_t * fd_iter_next(fd_t *current, strseg_t from_name) {
	RBTreeNode *node;
	if (current) {
		node= RBTreeNode_GetNext(&current->name_index_node);
	} else {
		RBTreeSearch s= RBTree_Find( &fd_by_name_index, (void*) &from_name );
		if (s.Nearest == NULL)
			node= NULL;
		else if (s.Relation > 0)
			node= s.Nearest;
		else
			node= RBTreeNode_GetNext(s.Nearest);
	}
	return node? (fd_t *) node->Object : NULL;
}
