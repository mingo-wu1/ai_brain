#include <stdio.h>
#include "../include/b_tree.h"

int main(int argc, char** argv){
	Balanced_Multiple_Search_Tree btree;;
	btree.insert(1,10);
	btree.insert(2,100);
	btree.insert(3,108);
	btree.insert(5,109);
	btree.insert(6,102);
	btree.insert(9,101);
	printf("%d\n",btree.search(9));
	return 0;
}