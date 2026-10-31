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
#include "pv.h"
#include "search.h"
#include <stdatomic.h>

#define QS_DELTA_MARGIN 200
#define QS_QUIET_EVASIONS 3

int qsearch(const int ply, int alpha, const int beta) {

  Node *node = &nodes[ply];
  pv_len[ply] = 0;
  const Position *pos = &node->pos;

  if (ply >= MAX_PLY-1) {
    lazy_update_accs(node);
    return net_eval(node);
  }

  const int stm = pos->stm;
  const int opp = stm ^ 1;
  const int stm_king_idx = piece_index(KING, stm);
  const int in_check = is_attacked(pos, bsf(pos->all[stm_king_idx]), opp);
  const int is_pv = beta - alpha != 1;

  TimeControl *tc = &time_control;
  count_node(ply);

  TTHit tte;
  const int tt_hit = tt_probe(pos->hash, &tte);
  const int tt_score = tt_hit ? get_adjusted_score(ply, tte.score) : 0;

  if (!is_pv && tt_hit && (tte.bound & (tt_score >= beta ? TT_BETA : TT_ALPHA))) {
    return tt_score;
  }

  lazy_update_accs(node);

  int raw_ev = TT_NO_EVAL;
  int stand_pat = 0;
  int best_score = -MATE + ply;

  if (!in_check) {

    raw_ev = (tt_hit && tte.eval != TT_NO_EVAL) ? tte.eval : net_eval(node);
    stand_pat = correct_eval(pos, raw_ev);

    // a stored search result is a better guess than the static eval when it
    // bounds the eval from the right side
    if (tt_hit && tt_score > -MATEISH && tt_score < MATEISH &&
        (tte.bound & (tt_score > stand_pat ? TT_BETA : TT_ALPHA))) {
      stand_pat = tt_score;
    }

    if (stand_pat >= beta) {
      if (!tt_hit)
        tt_put(pos->hash, TT_BETA, 0, put_adjusted_score(ply, stand_pat), raw_ev, 0, 0);
      return stand_pat;
    }

    if (stand_pat > alpha)
      alpha = stand_pat;

    best_score = stand_pat;

  }

  const int orig_alpha = alpha;

  // out of check only captures and queen promotions are searched here
  move_t tt_move = tt_hit ? tte.move : 0;
  if (!in_check && (!(tt_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE)) ||
                    ((tt_move & MOVE_FLAG_PROMOTE) && ((tt_move >> 12) & 7) != QUEEN)))
    tt_move = 0;

  Node *next_node = &nodes[ply + 1];
  Position *next_pos = &next_node->pos;
  const uint64_t *next_stm_king_ptr = &next_pos->all[stm_king_idx];
  move_t move;
  move_t best_move = 0;
  int played = 0;
  int quiet_evasions = 0;

  node->cont2 = NULL;
  node->cont4 = NULL;

  init_next_qsearch_move(node, in_check, tt_move);

  while ((move = get_next_qsearch_move(node))) {

    const int is_quiet = !(move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE));

    if (best_score > -MATEISH) {

      if (is_quiet && quiet_evasions >= QS_QUIET_EVASIONS)
        continue;

      if (!in_check && !(move & (MOVE_FLAG_PROMOTE | MOVE_FLAG_EPCAPTURE))) {
        const int gain = stand_pat + see_values[pos->board[move & 0x3F]] + QS_DELTA_MARGIN;
        if (gain <= alpha) {
          if (gain > best_score)
            best_score = gain;
          continue;
        }
      }

      if (!see_ge(pos, move, 0))
        continue;

    }

    pos_copy(pos, next_pos);
    make_move(next_node, move);
    tt_prefetch(next_pos->hash);
    if (is_attacked(next_pos, bsf(*next_stm_king_ptr), opp))
      continue;

    next_node->accs_dirty = 1;
    next_node->cont_entry = NULL;
    next_node->prev_piece = EMPTY;
    played++;
    if (is_quiet)
      quiet_evasions++;

    const int score = -qsearch(ply+1, -beta, -alpha);

    if (tc->finished)
      return 0;

    if (score > best_score) {
      best_score = score;
      if (score > alpha) {
        best_move = move;
        alpha = score;
        if (score >= beta) {
          tt_put(pos->hash, TT_BETA, 0, put_adjusted_score(ply, best_score), raw_ev, best_move, 0);
          return score;
        }
      }
    }

  }

  if (in_check && !played)
    return -MATE + ply;

  tt_put(pos->hash, (alpha > orig_alpha) ? TT_EXACT : TT_ALPHA, 0, put_adjusted_score(ply, best_score), raw_ev, best_move, 0);

  return best_score;

}
