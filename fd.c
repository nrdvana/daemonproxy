#include "config.h"
#include "init-frame.h"

// Describes a named file handle
typedef struct fd_s {
	RBTreeNode name_index_node;
	int fd;
	bool has_path;
	char buffer[];
} fd_t;

// Allocated once, lazily, and then treated as a fixed-size allocation pool
fd_t *fd_pool= NULL;
RBTreeNode fd_by_name_index;
fd_t *fd_free_list;

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

