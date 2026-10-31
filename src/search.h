#ifndef SEARCH_H
#define SEARCH_H

#include <stdint.h>
#include "move.h"

extern _Thread_local int local_node_batch;
extern _Thread_local int current_thread_id;
extern _Thread_local uint64_t thread_nodes;
extern _Thread_local int sel_depth;

void init_lmr(void);
void search_thread_init(int thread_id);
void flush_local_nodes(void);
uint64_t root_move_nodes(const move_t move);
int search(const int ply, int depth, int alpha, int beta);

#endif
