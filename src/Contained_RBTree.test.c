#include "config.h"
#include "Contained_RBTree.h"
#include "assert.h"

// Some quick and dirty test cases for Contained_RBTree

struct Obj {
	RBTreeNode node;
	int value;
};

int compare_fn(void *data, RBTreeNode *node) {
	int a= *((int*)data);
	int b= ((struct Obj*)node->Object)->value;
	return a < b? -1 : a > b? 1 : 0;
}

int main() {
	RBTree tree;
	RBTree_Init(&tree, compare_fn);
	struct Obj objs[500];
	int i;
	
	// insert all nodes
	for (i=0; i < 500; i++) {
		RBTreeNode_Init(&objs[i].node);
		objs[i].node.Object= &objs[i];
		objs[i].value= i;
		RBTree_Add(&tree, &objs[i].node, (void*) &i);
	}
	// delete even numbered nodes
	for (i=0; i < 500; i+= 2) {
		RBTreeSearch s= RBTree_Find(&tree, (void*) &i);
		assert(s.Relation == 0);
		assert(s.Nearest != NULL);
		RBTreeNode_Prune(s.Nearest);
	}
	// inspect the total list
	RBTreeNode *cur= RBTree_GetFirst(&tree);
	int n= 0;
	while (cur) {
		n++;
		assert(((struct Obj*)cur->Object)->value & 1); // must be odd
		cur= RBTreeNode_GetNext(cur);
	}
	assert(n == 250);
	// re-insert even nubered nodes
	for (i= 0; i < 500; i+= 2)
		RBTree_Add(&tree, &objs[i].node, (void*) &i);
	// delete odd numbered nodes
	for (i= 1; i < 500; i+= 2) {
		RBTreeSearch s= RBTree_Find(&tree, (void*) &i);
		assert(s.Relation == 0);
		assert(s.Nearest != NULL);
		RBTreeNode_Prune(s.Nearest);
	}
	// inspect total list
	cur= RBTree_GetFirst(&tree);
	n= 0;
	while (cur) {
		n++;
		assert((((struct Obj*)cur->Object)->value & 1) == 0); // must be even
		cur= RBTreeNode_GetNext(cur);
	}
	assert(n == 250);
	// delete remaining (even) nodes in reverse, until tree is empty
	for (i=0; i < 250; i++) {
		RBTreeNode *last= RBTree_GetLast(&tree);
		assert(last != NULL);
		RBTreeNode_Prune(last);
	}
	assert(RBTree_GetFirst(&tree) == NULL);
	return 0;
}