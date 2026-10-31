#include "types.h"
#include "timecontrol.h"
#include "move.h"
#include "nodes.h"
#include "search.h"

TimeControl time_control;
int move_overhead = 10;

static inline double min_d(const double a, const double b) {
  return a < b ? a : b;
}

void init_tc(int64_t wtime, int64_t winc, int64_t btime, int64_t binc, int64_t max_nodes, int64_t move_time, int max_depth, int moves_to_go, int infinite) {

  TimeControl *tc = &time_control;

  if (wtime < 0) wtime = 0;
  if (winc < 0) winc = 0;
  if (btime < 0) btime = 0;
  if (binc < 0) binc = 0;
  if (max_nodes < 0) max_nodes = 0;
  if (move_time < 0) move_time = 0;
  if (moves_to_go < 0) moves_to_go = 0;
  if (max_depth < 0) max_depth = 0;

  const int stm = nodes[0].pos.stm;  // assume a position command has been issued
  const int64_t our_time = stm == WHITE ? wtime : btime;
  const int64_t our_inc = stm == WHITE ? winc : binc;

  if (!infinite && !max_nodes && !max_depth && !wtime && !winc && !btime && !binc && !move_time)
    move_time = 100;

  if (!max_depth || max_depth >= MAX_PLY)
    max_depth = MAX_PLY - 1;

  tc->start_time = time_ms();
  tc->finish_time = 0;
  tc->soft_ms = 0;

  if (move_time) {
    const int64_t usable = move_time - move_overhead;
    tc->finish_time = tc->start_time + (uint64_t)(usable > 1 ? usable : 1);
  }
  else if (our_time || our_inc) {
    // The increment only arrives after this move, so nothing here may depend on
    // it being spendable: both limits are capped by what's on the clock.
    const int64_t usable = our_time - move_overhead > 1 ? our_time - move_overhead : 1;
    const double base = moves_to_go ? (double)usable / moves_to_go + 0.85 * (double)our_inc
                                    : 0.054 * (double)usable + 0.85 * (double)our_inc;
    const double cap = 0.76 * (double)usable;
    const double hard = min_d(3.04 * base, cap);
    const double soft = min_d(0.76 * base, cap);
    tc->finish_time = tc->start_time + (uint64_t)(hard > 1 ? hard : 1);
    tc->soft_ms = (uint64_t)(soft > 1 ? soft : 1);
  }

  tc->max_nodes = (uint64_t)max_nodes;
  tc->soft_nodes = 0;
  tc->max_depth = max_depth;
  tc->infinite = infinite;

  tc->nodes = 0;
  tc->best_move = 0;
  tc->best_score = 0;
  tc->stability = 0;
  tc->prev_score = 0;
  tc->prev_best = 0;
  tc->finished = 0;

}

void check_tc_nodes(void) {

  TimeControl *tc = &time_control;

  if (tc->finished)
    return;

  if (tc->max_nodes && tc->nodes >= tc->max_nodes) {
    tc->finished = 1;
    return;
  }

  if (tc->finish_time && current_thread_id == 0 && time_ms() >= tc->finish_time)
    tc->finished = 1;

}

// Called by the main thread after each completed iteration. Returns 1 when
// another iteration isn't worth starting: spend longer when the best move keeps
// changing, the score is falling, or the best move has little of the node count.
int tc_iteration_done(int depth, move_t best, int score, uint64_t best_move_nodes, uint64_t thread_nodes) {

  static const double stability_scale[5] = {2.43, 1.35, 1.09, 0.88, 0.68};
  static const double score_scale[5] = {1.25, 1.15, 1.00, 0.94, 0.88};

  TimeControl *tc = &time_control;

  if (tc->soft_nodes && tc->nodes >= tc->soft_nodes)
    return 1;

  tc->stability = (best == tc->prev_best) ? (tc->stability < 4 ? tc->stability + 1 : 4) : 0;
  tc->prev_best = best;

  const int score_drop = tc->prev_score - score;
  tc->prev_score = score;

  if (!tc->soft_ms)
    return 0;

  double scale = 1.0;

  if (depth >= 7) {
    const int band = score_drop > 30 ? 0 : score_drop > 10 ? 1 : score_drop > -10 ? 2 : score_drop > -30 ? 3 : 4;
    const double fraction = thread_nodes ? (double)best_move_nodes / (double)thread_nodes : 1.0;
    scale = stability_scale[tc->stability] * score_scale[band] * (1.52 - fraction) * 1.74;
  }

  return (double)(time_ms() - tc->start_time) >= (double)tc->soft_ms * scale;

}
