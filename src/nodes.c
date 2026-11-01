#include "nodes.h"

_Thread_local Node nodes[MAX_PLY];

void clear_nodes(void) {

  for (int i=0; i < MAX_PLY; i++) {

    Node *node = &nodes[i];

    node->killer = 0;
    node->killer2 = 0;
    node->prev_move = 0;
    node->cutoffs = 0;
    node->cont_entry = NULL;
    node->cont2 = NULL;
    node->cont4 = NULL;
    node->excluded_move = 0;
    node->dextensions = 0;
    node->ev = 0;
    node->prev_piece = EMPTY;
    node->prev_to = 0;

  }  

}
