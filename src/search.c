#include <stdio.h>
#include <string.h>
#include <math.h>
#include "types.h"
#include "builtins.h"
#include "nodes.h"
#include "pos.h"
#include "evaluate.h"
#include "search.h"
#include "makemove.h"
#include "timecontrol.h"
#include "iterate.h"
#include "move.h"
#include "qsearch.h"
#include "net.h"
#include "tt.h"
#include "hh.h"
#include "history.h"
#include "corrhist.h"
#include "pv.h"
#include "see.h"
#include "tune.h"
#include <stdatomic.h>

_Thread_local int local_node_batch = 0;
_Thread_local int current_thread_id = 0;
_Thread_local uint64_t thread_nodes = 0;
_Thread_local int sel_depth = 0;

static _Thread_local uint64_t root_nodes[64][64];

// Null move pruning stays off below this ply while its result is being verified.
static _Thread_local int nmp_min_ply = 0;

void search_thread_init(int thread_id) {
  current_thread_id = thread_id;
  local_node_batch = 0;
  thread_nodes = 0;
  sel_depth = 0;
  nmp_min_ply = 0;
  memset(root_nodes, 0, sizeof(root_nodes));
}

void flush_local_nodes(void) {
  if (local_node_batch > 0) {
    atomic_fetch_add_explicit(&time_control.nodes, (uint64_t)local_node_batch, memory_order_relaxed);
    local_node_batch = 0;
  }
}

uint64_t root_move_nodes(const move_t move) {
  return root_nodes[(move >> 6) & 0x3F][move & 0x3F];
}

static int lmr[MAX_PLY][MAX_MOVES];

void init_lmr(void) {
  for (int d = 1; d < MAX_PLY; d++) {
    for (int m = 1; m < MAX_MOVES; m++) {
      lmr[d][m] = lmr_base / 100.0 + log(d) * log(m < 64 ? m : 63) / (lmr_div / 100.0);
    }
  }
}

static inline int is_mate_score(const int score) {
  return score <= -MATEISH || score >= MATEISH;
}

// Whether the static eval is better than it was at our previous turn.
static inline int is_improving(const int ply, const int ev) {
  if (ply >= 2 && nodes[ply-2].ev != TT_NO_EVAL)
    return ev > nodes[ply-2].ev;
  if (ply >= 4 && nodes[ply-4].ev != TT_NO_EVAL)
    return ev > nodes[ply-4].ev;
  return 1;
}

// Zugzwang is only a real risk when the side to move has just king and pawns.
static inline int has_non_pawn_material(const Position *pos, const int stm) {
  return (pos->colour[stm] & ~(pos->all[piece_index(PAWN, stm)] | pos->all[piece_index(KING, stm)])) != 0;
}

static inline void enter_child(Node *next_node, const int moved_piece, const int to) {
  next_node->accs_dirty = 1;
  next_node->cont_entry = cont_history[moved_piece][to];
  next_node->prev_piece = moved_piece;
  next_node->prev_to = to;
}

int search(const int ply, int depth, int alpha, int beta, const int cutnode) {

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
  const int is_root = ply == 0;
  const int is_pv = is_root || (beta - alpha != 1);
  const move_t excluded = node->excluded_move;

  if (!is_root) {
    hh_store(ply, pos->hash);
    if (is_draw(ply, pos->hash, pos->hmc) || is_mat_draw(pos)) {
      // checkmate on the move that reaches the 50 move limit still counts as mate
      if (pos->hmc >= 100 && in_check && !first_legal_move(node))
        return -MATE + ply;
      return 0;
    }
  }

  node->dextensions = is_root ? 0 : nodes[ply-1].dextensions;
  node->cont2 = ply >= 1 ? nodes[ply-1].cont_entry : NULL;
  node->cont4 = ply >= 3 ? nodes[ply-3].cont_entry : NULL;

  if (depth <= 0 && !in_check) {
    return qsearch(ply, alpha, beta);
  }
  if (depth < 0)
    depth = 0;

  TimeControl *tc = &time_control;
  count_node(ply);

  if (alpha < -MATE + ply)
    alpha = -MATE + ply;
  if (beta > MATE - ply - 1)
    beta = MATE - ply - 1;
  if (alpha >= beta)
    return alpha;

  TTHit tte;
  const int tt_hit = tt_probe(pos->hash, &tte);
  const move_t tt_move = tt_hit ? tte.move : 0;
  const int tt_depth = tt_hit ? tte.depth : 0;
  const int tt_bound = tt_hit ? tte.bound : TT_NONE;
  const int tt_score = tt_hit ? get_adjusted_score(ply, tte.score) : 0;
  const int tt_pv = is_pv || (tt_hit && tte.pv);
  const int tt_noisy = (tt_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE)) != 0;

  if (!is_pv && !excluded && tt_hit && tt_depth >= depth && pos->hmc < 90 &&
      (tt_bound & (tt_score >= beta ? TT_BETA : TT_ALPHA))) {
    return tt_score;
  }

  if (!in_check && !excluded && depth >= 4 && (!tt_move || tt_depth + 4 < depth)) {
    depth--;
  }

  int raw_ev = TT_NO_EVAL;

  if (in_check) {
    node->ev = TT_NO_EVAL;
  }
  else if (!excluded) {  // the excluded search shares the eval of this same node
    if (tt_hit && tte.eval != TT_NO_EVAL) {
      raw_ev = tte.eval;
    }
    else {
      lazy_update_accs(node);
      raw_ev = net_eval(node);
    }
    node->ev = correct_eval(node, raw_ev);
  }

  const int static_ev = node->ev;
  const int improving = !in_check && is_improving(ply, static_ev);

  // a stored search result is a better guess than the static eval when it bounds
  // the eval from the right side
  int ev = static_ev;
  if (!in_check && !excluded && tt_hit && !is_mate_score(tt_score) &&
      (tt_bound & (tt_score > static_ev ? TT_BETA : TT_ALPHA))) {
    ev = tt_score;
  }

  Node *next_node = &nodes[ply + 1];
  Position *next_pos = &next_node->pos;

  if (!is_pv && !excluded && !in_check) {

    // Reverse futility: too far above beta for a quiet reply to matter
    if (depth <= 8 && !is_mate_score(ev) && beta > -MATEISH && ev >= beta + rfp_margin * (depth - improving)) {
      return ev;
    }

    // Razoring: too far below alpha for anything but a tactic to save it
    if (depth <= 3 && alpha < MATEISH && ev + razor_base + depth * razor_mul <= alpha) {
      const int razor_score = qsearch(ply, alpha, alpha + 1);
      if (tc->finished)
        return 0;
      if (razor_score <= alpha) {
        return razor_score;
      }
    }

    // Null move: if passing still beats beta, a real move almost surely will
    if (depth > 2 && ev >= beta && static_ev >= beta - nmp_static_margin && beta > -MATEISH && node->cont_entry &&
        ply >= nmp_min_ply && has_non_pawn_material(pos, stm) &&
        !(tt_hit && tt_bound == TT_ALPHA && tt_score < beta)) {

      const int surplus = (ev - beta) / nmp_eval_div;
      const int reduction = 4 + depth / nmp_depth_div + (surplus < 3 ? surplus : 3);
      const int nmp_depth = depth - reduction;

      lazy_update_accs(node);
      pos_copy(pos, next_pos);
      make_null_move(next_pos);
      tt_prefetch(next_pos->hash);
      memcpy(next_node->accs, node->accs, sizeof(node->accs));
      next_node->accs_dirty = 0;
      next_node->cont_entry = NULL;
      next_node->prev_piece = EMPTY;
      next_node->prev_to = 0;

      const int score = -search(ply+1, nmp_depth, -beta, -beta+1, !cutnode);

      if (tc->finished)
        return 0;

      if (score >= beta) {

        const int result = score > MATEISH ? beta : score;

        if (nmp_min_ply || depth < 14)
          return result;

        // At high depth verify with null move switched off, to catch zugzwang
        nmp_min_ply = ply + 3 * nmp_depth / 4;
        const int verified = search(ply, nmp_depth, beta - 1, beta, 0);
        nmp_min_ply = 0;

        if (tc->finished)
          return 0;

        if (verified >= beta)
          return result;

      }

    }

    // ProbCut: a good capture that beats beta by a margin at reduced depth
    const int pc_beta = beta + pc_margin - pc_improving * improving;

    if (depth >= 5 && !is_mate_score(beta) && !(tt_hit && tt_depth >= depth - 3 && tt_score < pc_beta)) {

      const int threshold = pc_beta - static_ev;
      move_t move;

      lazy_update_accs(node);

      init_next_qsearch_move(node, 0, (tt_noisy && (!(tt_move & MOVE_FLAG_PROMOTE) || ((tt_move >> 12) & 7) == QUEEN)) ? tt_move : 0);

      while ((move = get_next_qsearch_move(node))) {

        if (!see_ge(pos, move, threshold))
          continue;

        const int from = (move >> 6) & 0x3F;
        const int to = move & 0x3F;
        const int moved_piece = pos->board[from];

        pos_copy(pos, next_pos);
        make_move(next_node, move);
        tt_prefetch(next_pos->hash);
        if (is_attacked(next_pos, bsf(next_pos->all[stm_king_idx]), opp))
          continue;

        enter_child(next_node, moved_piece, to);

        int score = -qsearch(ply+1, -pc_beta, -pc_beta+1);

        if (!tc->finished && score >= pc_beta)
          score = -search(ply+1, depth-4, -pc_beta, -pc_beta+1, !cutnode);

        if (tc->finished)
          return 0;

        if (score >= pc_beta) {
          tt_put(pos->hash, TT_BETA, depth - 3, put_adjusted_score(ply, score), raw_ev, move, tt_pv);
          return score;
        }

      }

    }

  }

  move_t move = 0;
  move_t best_move = 0;
  int score = 0;
  int best_score = -INF;
  int played = 0;
  const uint64_t *next_stm_king_ptr = &next_pos->all[stm_king_idx];
  const int orig_alpha = alpha;
  const int lmr_row = depth < MAX_PLY ? depth : MAX_PLY - 1;

  lazy_update_accs(node);  // children update from this node's accumulators

  init_next_search_move(node, in_check, tt_move);

  while ((move = get_next_search_move(node))) {

    if (move == excluded)
      continue;

    const int is_quiet = !(move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE));
    const int from = (move >> 6) & 0x3F;
    const int to = move & 0x3F;
    const int moved_piece = pos->board[from];
    const int hist = is_quiet ? quiet_history(node, stm, moved_piece, from, to) : 0;
    const int base_reduction = lmr[lmr_row][played + 1 < MAX_MOVES ? played + 1 : MAX_MOVES - 1];

    // Shallow move pruning, once at least one line has avoided being mated
    if (!is_root && best_score > -MATEISH) {

      const int lmr_depth = depth - 1 - base_reduction > 0 ? depth - 1 - base_reduction : 0;

      if (is_quiet) {

        if (!in_check && depth <= 8 && played >= (lmp_base + depth * depth) / (2 - improving)) {
          node->skip_quiets = 1;
          continue;
        }

        if (!in_check && lmr_depth <= 8 && static_ev + fut_base + fut_mul * lmr_depth <= alpha) {
          node->skip_quiets = 1;
          continue;
        }

        if (depth <= 6 && hist < -hist_prune * depth)
          continue;

        if (lmr_depth <= 8 && !see_ge(pos, move, -see_quiet * lmr_depth * lmr_depth))
          continue;

      }
      else {

        if (!in_check && depth <= 6 && !(move & (MOVE_FLAG_PROMOTE | MOVE_FLAG_EPCAPTURE)) &&
            static_ev + see_values[pos->board[to]] + cap_fut_base + cap_fut_mul * depth <= alpha)
          continue;

        if (depth <= 8 && !see_ge(pos, move, -see_noisy * depth))
          continue;

      }

    }

    int extension = 0;
    if (!is_root && !excluded && depth >= 8 && move == tt_move && tt_depth + 4 >= depth && (tt_bound & TT_BETA) && !is_mate_score(tt_score)) {

      const int s_beta = tt_score - se_margin * depth;
      const int s_depth = (depth - 1) / 2;

      node->excluded_move = tt_move;
      const int s_score = search(ply, s_depth, s_beta - 1, s_beta, cutnode);
      node->excluded_move = 0;
      restart_after_tt_move(node, tt_move);

      if (tc->finished)
        return 0;

      if (s_score < s_beta) {
        extension = 1;
        if (!is_pv && s_score < s_beta - dext_margin && node->dextensions <= 6)
          extension = 2;
      }
      else if (s_beta >= beta)
        return s_beta;
      else if (tt_score >= beta || cutnode)
        extension = -1;
    }

    const int was_bad_noisy = is_bad_noisy_stage(node);

    pos_copy(pos, next_pos);
    make_move(next_node, move);
    tt_prefetch(next_pos->hash);
    if (is_attacked(next_pos, bsf(*next_stm_king_ptr), opp))
      continue;

    enter_child(next_node, moved_piece, to);

    node->played[played++] = move;

    const int new_depth = depth - 1 + extension;
    const uint64_t nodes_before = thread_nodes;

    if (extension == 2)
      node->dextensions++;

    // Late move reductions: search late quiets and losing captures shallower,
    // then properly if that surprises us
    if (depth >= 3 && played >= (is_pv ? 3 : 2) && (is_quiet || was_bad_noisy)) {

      int reduction = base_reduction;

      if (is_quiet) {
        reduction += !tt_pv;
        reduction += !improving;
        reduction += cutnode;
        reduction += tt_noisy;
        reduction -= is_attacked(next_pos, bsf(next_pos->all[piece_index(KING, opp)]), stm);
        reduction -= hist / lmr_hist_div;
      }
      else {
        reduction = reduction / 2 + !tt_pv;
      }

      int d = new_depth - reduction;
      d = d < 1 ? 1 : (d > new_depth ? new_depth : d);

      score = -search(ply+1, d, -alpha-1, -alpha, 1);

      if (!tc->finished && score > alpha && d < new_depth) {

        // how far above alpha it came back decides how much deeper to look
        const int full_depth = new_depth + (score > best_score + lmr_deeper + 2 * new_depth) - (score < best_score + new_depth);

        if (d < full_depth)
          score = -search(ply+1, full_depth, -alpha-1, -alpha, !cutnode);

      }

    }
    else if (!is_pv || played > 1) {
      score = -search(ply+1, new_depth, -alpha-1, -alpha, !cutnode);
    }

    if (is_pv && !tc->finished && (played == 1 || score > alpha)) {
      score = -search(ply+1, new_depth, -beta, -alpha, 0);
    }

    if (extension == 2)
      node->dextensions--;

    if (is_root)
      root_nodes[from][to] += thread_nodes - nodes_before;

    if (tc->finished)
      return 0;

    if (score > best_score) {
      best_score = score;
      best_move = move;
      if (score > alpha) {
        alpha = score;
        if (is_root && current_thread_id == 0) {
          tc->best_move = best_move;
          tc->best_score = best_score;
        }
        if (is_pv) {
          collect_pv(ply, best_move);
        }
        if (score >= beta) {

          // How far the move beat the static eval scales the history bonus
          int stat_diff = in_check ? 0 : score - static_ev;
          if (stat_diff < 0) stat_diff = 0;

          int bonus = (bonus_mul * depth * depth) + (stat_diff * depth / bonus_diff_div);
          if (bonus > bonus_max)
            bonus = bonus_max;

          for (int i=0; i < played; i++) {
            const move_t pm = node->played[i];
            if (pm & MOVE_FLAG_CAPTURE)
              update_capture_history(pos, pm, pm == best_move ? bonus : -bonus);
          }
          if (!(best_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE))) {
            update_killer(node, best_move);
            update_quiet_histories(node, pos, best_move, bonus);

            if (ply > 0 && node->prev_piece != EMPTY) {
              counter_moves[node->prev_piece][node->prev_to] = best_move;
            }

            for (int i=0; i < played-1; i++) {
              const move_t pm = node->played[i];
              if (!(pm & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE))) {
                update_quiet_histories(node, pos, pm, -bonus);
              }
            }
          }

          if (!excluded && !in_check && best_score < MATEISH && best_score > static_ev && !(best_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE)))
            update_corrhist(node, depth, best_score - static_ev);
          if (!excluded)
            tt_put(pos->hash, TT_BETA, depth, put_adjusted_score(ply, best_score), raw_ev, best_move, tt_pv);
          return score;
        }
      }
    }
  }

  if (played == 0) {
    // with a move excluded this says nothing about mate or stalemate
    if (excluded)
      return alpha;
    return in_check ? (-MATE + ply) : 0;
  }

  if (!excluded && !in_check && !is_mate_score(best_score) && !(best_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE)) && !(alpha == orig_alpha && best_score >= static_ev))
    update_corrhist(node, depth, best_score - static_ev);

  if (!excluded)
    tt_put(pos->hash, (alpha > orig_alpha) ? TT_EXACT : TT_ALPHA, depth, put_adjusted_score(ply, best_score), raw_ev, best_move, tt_pv);

  return best_score;

}
