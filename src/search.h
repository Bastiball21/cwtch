#ifndef SEARCH_H
#define SEARCH_H

#include <stdatomic.h>
#include <stdint.h>
#include "move.h"
#include "timecontrol.h"

extern _Thread_local int local_node_batch;
extern _Thread_local int current_thread_id;
extern _Thread_local uint64_t thread_nodes;
extern _Thread_local int sel_depth;

static inline void count_node(const int ply) {
  local_node_batch++;
  thread_nodes++;
  if (ply > sel_depth)
    sel_depth = ply;
  if (local_node_batch >= 1024) {
    atomic_fetch_add_explicit(&time_control.nodes, 1024, memory_order_relaxed);
    local_node_batch = 0;
    check_tc_nodes();
  }
}

void init_lmr(void);
void search_thread_init(int thread_id);
void flush_local_nodes(void);
uint64_t root_move_nodes(const move_t move);
int search(const int ply, int depth, int alpha, int beta, const int cutnode);

#endif
