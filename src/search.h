#ifndef SEARCH_H
#define SEARCH_H

extern _Thread_local int local_node_batch;
extern _Thread_local int current_thread_id;

void init_lmr(void);
void reset_local_nodes(void);
void flush_local_nodes(void);
int search(const int ply, int depth, int alpha, int beta);

#endif