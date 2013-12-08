#include "config.h"
#include "init-frame.h"
#include "Contained_RBTree.h"

// Describes a named file handle

#define FD_TYPE_UNDEF  0
#define FD_TYPE_FILE   1
#define FD_TYPE_PIPE_R 2
#define FD_TYPE_PIPE_W 3

struct fd_s {
	int size;
	int type;
	RBTreeNode name_index_node;
	int fd;
	union {
		char *path;
		struct fd_s *pipe_peer;
		struct fd_s *next_free;
	};
	char buffer[];
};

fd_t *fd_pool= NULL;
RBTreeNode fd_by_name_index;
fd_t *fd_free_list;

void add_fd_by_name(fd_t *fd);

void fd_build_pool(void *buffer, int fd_count, int size_each) {
	int i;
	fd_pool= (fd_t*) buffer;
	memset(buffer, 0, fd_count*size_each);
	fd_free_list= fd_pool;
	for (i=0; i < fd_count; i++) {
		fd_pool[i].size= size_each;
		fd_pool[i].next_free= fd_pool+i+1;
	}
	fd_pool[fd_count-1].next_free= NULL;
	RBTree_InitRootSentinel( &fd_by_name_index );
}

// Open a pipe from one named FD to another
// returns a ref to the read-end, which has a pointer to the write-end.
fd_t * fd_pipe(const char *name1, const char *name2) {
	int pair[2], n;
	fd_t *fd1, *fd2, *next;
	fd1= fd_by_name(name1);
	fd2= fd_by_name(name2);
	next= fd_free_list;
	// Check that we have name1, or available fd_t object and name fits in it.
	if (!fd1) {
		n= strlen(name1);
		if (!next || n >= next->size - sizeof(fd_t))
			return NULL;
		fd1= next;
		fd1->type= FD_TYPE_UNDEF;
		next= next->next_free;
		memcpy(fd1->buffer, name1, n+1);
	}
	// Check that we have name1, or available fd_t object and name fits in it.
	if (!fd2) {
		n= strlen(name2);
		if (!next || n >= next->size - sizeof(fd_t))
			return NULL;
		fd2= next;
		fd2->type= FD_TYPE_UNDEF;
		next= next->next_free;
		memcpy(fd2->buffer, name2, n+1);
	}
	// Check that pipe() call succeeds
	if (pipe(pair))
		return NULL;
	
	// Nothing else can fail beyond this point, so start committing our changes
	fd_free_list= next;
	
	// If fd1 is newly allocated, add it to the index.
	if (fd1->type == FD_TYPE_UNDEF)
		add_fd_by_name(fd1);
	// else close the file handle it previously referenced
	else if (fd1->fd >= 0)
		close(fd1->fd);
	// same for fd2
	if (fd2->type == FD_TYPE_UNDEF)
		add_fd_by_name(fd2);
	else if (fd2->fd >= 0)
		close(fd2->fd);
	
	fd1->type= FD_TYPE_PIPE_R;
	fd1->fd= pair[0];
	fd1->pipe_peer= fd2;
	fd2->type= FD_TYPE_PIPE_W;
	fd2->fd= pair[1];
	fd2->pipe_peer= fd1;
	return fd1;
}

// Open a file on the given name, possibly closing a handle by that name
fd_t * fd_open(const char *name, const char *path, const char *opts) {
	int flags, fd, n, n2;
	char *s;
	fd_t *fd_obj= fd_by_name(name);
	if (!fd_obj) {
		if (!fd_free_list)
			return NULL;
		n= strlen(name);
		if (n >= fd_free_list->size - sizeof(fd_t))
			return NULL;
		memcpy(fd_free_list->buffer, name, n+1);
	}
	
	// Now, try to perform the open
	flags= parse_open_flags(opts);
	if (flags & O_MKDIR_P)
		create_missing_dirs(path);
	fd= open(path, flags, 600);
	if (fd < 0)
		return NULL;
	
	// Overwrite (and possibly setup) the fd_t object
	if (!fd_obj) {
		fd_obj= fd_free_list;
		fd_free_list= fd_free_list->next_free;
		add_fd_by_name(fd_obj);
	} else {
		if (fd_obj->fd >= 0)
			close(fd_obj->fd);
	}
	fd_obj->fd= fd;
	fd_obj->type= FD_TYPE_FILE;
	
	// copy as much of path into the buffer as we can.
	n= strlen(name);
	fd_obj->path= fd_obj->buffer + n + 1;
	n2= strlen(path);
	if (n+1+n2+1 > fd_obj->size - sizeof(fd_t)) {
		// truncate with '...'
		n2= fd_obj->size - sizeof(fd_t) - n - 1 - 3 - 1;
		if (n2 < 0)
			fd_obj->path= NULL;
		else {
			memcpy(fd_obj->path, path, n2);
			memcpy(fd_obj->path+n2, "...", 4);
		}
	}
	else
		memcpy(fd_obj->path, path, n2+1);
	return fd_obj;
}

// Close a named handle
void fd_delete(fd_t *fd) {
	// disassociate from other end of pipe, if its a pipe.
	if (fd->type == FD_TYPE_PIPE_R || fd->type == FD_TYPE_PIPE_W) {
		if (fd->pipe_peer)
			fd->pipe_peer->pipe_peer= NULL;
	}
	// close descriptor.
	close(fd->fd);
	// Remove name from index
	RBTree_Prune( &fd->name_index_node );
	// Clear the type
	fd->type= FD_TYPE_UNDEF;
	// and add it to the free-list.
	fd->next_free= fd_free_list;
	fd_free_list= fd;
}

bool fd_by_name_inorder(const fd_t *a, const fd_t *b) {
	return strcmp(a->buffer, b->buffer) <= 0;
}

int fd_by_name_key_compare(const char *str, const fd_t *b) {
	return strcmp(str, b->buffer);
}

fd_t * fd_by_name(const char *name) {
	RBTreeNode *node;
	if (fd_pool
		&& (node= RBTree_Find( &fd_by_name_index, name,
			(RBTree_compare_func*) fd_by_name_key_compare )))
		return (fd_t*) node->Object;
	return NULL;
}

void add_fd_by_name(fd_t *fd) {
	RBTreeNode_Init( &fd->name_index_node );
	fd->name_index_node.Object= fd;
	RBTree_Add( &fd_by_name_index, &fd->name_index_node,
		(RBTree_inorder_func*) fd_by_name_inorder);
}