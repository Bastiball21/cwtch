#ifndef TIMECONTROL_H
#define TIMECONTROL_H

#include <stdint.h>
#include <stdatomic.h>
#include "move.h"

typedef struct {

  uint64_t start_time;
  uint64_t finish_time;  // hard limit, 0 if none
  uint64_t soft_ms;      // base time before starting another iteration, 0 if none
  int max_depth;         // iterative deepening depth
  uint64_t max_nodes;    // hard limit, 0 if none
  uint64_t soft_nodes;   // don't start another iteration past this, 0 if none
  int infinite;          // must keep bestmove back until stopped
  move_t best_move;
  int best_score;
  int stability;
  int prev_score;
  move_t prev_best;
  _Atomic uint64_t nodes;
  _Atomic int finished;

} TimeControl;

extern TimeControl time_control;
extern int move_overhead;

void init_tc(int64_t wtime, int64_t winc, int64_t btime, int64_t binc, int64_t max_nodes, int64_t move_time, int max_depth, int moves_to_go, int infinite);
void check_tc_nodes(void);
int tc_iteration_done(int depth, move_t best, int score, uint64_t best_move_nodes, uint64_t thread_nodes);

#endif
