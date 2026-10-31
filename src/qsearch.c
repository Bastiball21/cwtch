#include <string.h>
#include "types.h"
#include "builtins.h"
#include "nodes.h"
#include "pos.h"
#include "evaluate.h"
#include "qsearch.h"
#include "makemove.h"
#include "timecontrol.h"
#include "iterate.h"
#include "move.h"
#include "net.h"
#include "tt.h"
#include "corrhist.h"
#include "see.h"
#include "debug.h"
#include "pv.h"
#include "search.h"
#include <stdatomic.h>

int qsearch(const int ply, int alpha, const int beta) {

  Node *node = &nodes[ply];
  pv_len[ply] = 0;
  const Position *pos = &node->pos;

  //debug_verify(1, node, ply);

  if (ply >= MAX_PLY-1) {
    lazy_update_accs(node);
    return net_eval(node);
  }

  //if (is_mat_draw(pos))
    //return 0;

  const int stm = pos->stm;
  const int opp = stm ^ 1;
  const int stm_king_idx = piece_index(KING, stm);
  const int in_check = 0; //is_attacked(pos, bsf(pos->all[stm_king_idx]), opp); // for minor move gen captures optimisation only

  lazy_update_accs(node);
  int stand_pat = correct_eval(pos, net_eval(node));

  if (stand_pat >= beta) {
    return stand_pat;
  }
  if (stand_pat > alpha) {
    alpha = stand_pat;
  }

  TimeControl *tc = &time_control;
  local_node_batch++;

  if (local_node_batch >= 1024) {
    atomic_fetch_add_explicit(&time_control.nodes, 1024, memory_order_relaxed);
    local_node_batch = 0;
    check_tc_nodes();
  }

  const TT *entry = tt_get(pos);
  if (entry) {
    const int tt_flags = entry->flags;
    const int tt_score = get_adjusted_score(ply, entry->score);
    if (tt_flags == TT_EXACT || (tt_flags == TT_BETA && tt_score >= beta) || (tt_flags == TT_ALPHA && tt_score <= alpha)) {
      return tt_score;
    }
  }

  const move_t tt_move = entry && (entry->move & MOVE_FLAG_CAPTURE) ? entry->move : 0;
  Node *next_node = &nodes[ply + 1];
  Position *next_pos = &next_node->pos;
  move_t move;
  move_t best_move = 0;
  int score;
  int best_score = stand_pat;
  //int played = 0;
  const int orig_alpha = alpha;
  const uint64_t *next_stm_king_ptr = &next_pos->all[stm_king_idx];
  
  init_next_qsearch_move(node, in_check, tt_move);

  while ((move = get_next_qsearch_move(node))) {

    //if (!see_ge(pos, move, 0)) {
      //continue;
    //}

    pos_copy(pos, next_pos);
    make_move(next_node, move);
    tt_prefetch(next_pos->hash);
    if (is_attacked(next_pos, bsf(*next_stm_king_ptr), opp))
      continue;

    next_node->accs_dirty = 1;

    //played++;

    score = -qsearch(ply+1, -beta, -alpha);

    if (tc->finished)
      return 0;

    if (score > best_score) {
      best_score = score;
      best_move = move;
      if (score > alpha) {
        alpha = score;
        if (score >= beta) {
          tt_put(pos, TT_BETA, 0, put_adjusted_score(ply, best_score), best_move); 
          return score;
        }
      }
    }
  }

  tt_put(pos, (alpha > orig_alpha) ? TT_EXACT : TT_ALPHA, 0, put_adjusted_score(ply, best_score), best_move); 

  return best_score;

}
