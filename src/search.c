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
#include "debug.h"
#include "pv.h"
#include "see.h"
#include <stdatomic.h>

_Thread_local int local_node_batch = 0;
_Thread_local int current_thread_id = 0;
_Thread_local uint64_t thread_nodes = 0;
_Thread_local int sel_depth = 0;

static _Thread_local uint64_t root_nodes[64][64];

void search_thread_init(int thread_id) {
  current_thread_id = thread_id;
  local_node_batch = 0;
  thread_nodes = 0;
  sel_depth = 0;
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
      lmr[d][m] = 0.73 + log(d) * log(m < 64 ? m : 63) / 2.36;
    }
  }
}

int search(const int ply, int depth, int alpha, int beta) {

  Node *node = &nodes[ply];
  pv_len[ply] = 0;
  const Position *pos = &node->pos;

  if (ply >= MAX_PLY-1) {
    lazy_update_accs(node);
    return net_eval(node);
  }

  if (ply > 0) {
    hh_store(ply, pos->hash);
    if (is_draw(ply, pos->hash, pos->hmc) || is_mat_draw(pos)) {
      return 0;
    }
  }

  const int stm = pos->stm;
  const int opp = stm ^ 1;
  const int stm_king_idx = piece_index(KING, stm);
  const int in_check = is_attacked(pos, bsf(pos->all[stm_king_idx]), opp);
  const int is_root = ply == 0;
  const int is_pv = is_root || (beta - alpha != 1);
  const move_t excluded = node->excluded_move;
  node->dextensions = is_root ? 0 : nodes[ply-1].dextensions;

  if (depth <= 0 && in_check == 0) {
    return qsearch(ply, alpha, beta);
  }
  if (depth < 0)
    depth = 0;

  TimeControl *tc = &time_control;
  local_node_batch++;
  thread_nodes++;
  if (ply > sel_depth)
    sel_depth = ply;

  if (local_node_batch >= 1024) {
    atomic_fetch_add_explicit(&time_control.nodes, 1024, memory_order_relaxed);
    local_node_batch = 0;
    check_tc_nodes();
  }

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

  if (!is_pv && !excluded && tt_hit && tt_depth >= depth && (tt_bound & (tt_score >= beta ? TT_BETA : TT_ALPHA))) {
    return tt_score;
  }

  if (!in_check && !excluded && depth >= 4 && (!tt_move || tt_depth + 4 < depth)) {
    depth--;
  }

  lazy_update_accs(node);
  const int raw_ev = (tt_hit && tte.eval != TT_NO_EVAL) ? tte.eval : net_eval(node);
  const int16_t ev = correct_eval(pos, raw_ev);
  node->ev = ev;

  const int improving = !in_check && (ply < 2 || ev > nodes[ply-2].ev);

  if (!is_pv && !excluded && !in_check && depth <= 8 && ev >= beta + (75 * (depth - improving))) {
    return ev;
  }

  // =========================================================
  // PAWNOCCHIO IDEA #2: RAZORING
  // If our evaluation is absolutely terrible, do a quick QSearch
  // to prove the position is dead before wasting time on a full search.
  // =========================================================
  if (!is_pv && !excluded && !in_check && depth <= 3) {
    int razor_margin = 300 + (depth * 100);
    if (ev + razor_margin <= alpha) {
      int razor_score = qsearch(ply, alpha, alpha + 1);
      if (razor_score <= alpha) {
        return razor_score;
      }
    }
  }
  // =========================================================

  Node *next_node = &nodes[ply + 1];
  Position *next_pos = &next_node->pos;

  if (!is_pv && !excluded && !in_check && depth > 2 && ev > beta && !is_pawn_endgame(pos)) {
  
    const int nmp_depth = depth - 3 - depth / 6;
  
    pos_copy(pos, next_pos);
    make_null_move(next_pos);
    tt_prefetch(next_pos->hash);
    memcpy(next_node->accs, node->accs, sizeof(node->accs));
    next_node->accs_dirty = 0;
    next_node->cont_entry = NULL;
    
    // Clear history tracking for null moves
    next_node->prev_piece = EMPTY;
    next_node->prev_to = 0;

    const int score = -search(ply+1, nmp_depth, -beta, -beta+1);
  
    if (score >= beta)
      return score > MATEISH ? beta : score;
  
    if (tc->finished)
      return 0;
  
  }

  move_t move = 0;
  move_t best_move = 0;
  int score = 0;
  int best_score = -INF;
  int played = 0;
  const uint64_t *next_stm_king_ptr = &next_pos->all[stm_king_idx];
  const int orig_alpha = alpha;

  init_next_search_move(node, in_check, tt_move);

  while ((move = get_next_search_move(node))) {

    if (move == excluded)
      continue;

    const int is_quiet = !(move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE));

    if (is_quiet && !is_pv && !in_check && alpha > -MATEISH && depth <= 2 && played > (6 * depth))
      continue;

    if (is_quiet && !is_pv && !in_check && alpha > -MATEISH && depth <= 4 && played && (ev + depth * 129) < alpha)
      continue;

    if (!is_quiet && !is_pv && !in_check && alpha > -MATEISH && depth <= 2 && played && !see_ge(pos, move, 0))
      continue;

    int extension = 0;
    if (!is_root && !excluded && depth >= 8 && move == tt_move && tt_depth + 4 >= depth && (tt_bound & TT_BETA) && tt_score > -MATEISH && tt_score < MATEISH) {

      const int s_beta = tt_score - 4 * depth;
      const int s_depth = (depth - 1) / 2;

      node->excluded_move = tt_move;
      const int s_score = search(ply, s_depth, s_beta - 1, s_beta);
      node->excluded_move = 0;
      node->stage = 1;          
      node->tt_move = tt_move;  

      if (tc->finished)
        return 0;

      if (s_score < s_beta) {     
        extension = 1;
        if (!is_pv && s_score < s_beta - 16 && node->dextensions <= 6)
          extension = 2;          
      }
      else if (s_beta >= beta)    
        return s_beta;
      else if (tt_score >= beta)  
        extension = -1;
    }

    const int from = (move >> 6) & 0x3F;
    const int to = move & 0x3F;
    const int moved_piece = pos->board[from];
    const int hist = is_quiet ? piece_to_history[moved_piece][to] : 0;

    pos_copy(pos, next_pos);
    make_move(next_node, move);
    tt_prefetch(next_pos->hash);
    if (is_attacked(next_pos, bsf(*next_stm_king_ptr), opp))
      continue;

    next_node->accs_dirty = 1;
    next_node->cont_entry = cont_history[moved_piece][to];
    
    // Assign previous move traits to the child ply
    next_node->prev_piece = moved_piece;
    next_node->prev_to = to;

    node->played[played++] = move;

    const int new_depth = depth - 1 + extension;
    const uint64_t nodes_before = thread_nodes;

    if (extension == 2)
      node->dextensions++;

    if (is_pv) {
      if (played == 1) { 
        score = -search(ply+1, new_depth, -beta, -alpha);
      }
      else { 

        int d = new_depth;

        if (depth >= 3 && played >= 3 && is_quiet && !in_check) {
          int reduction = lmr[depth < MAX_PLY ? depth : MAX_PLY - 1][played];
          reduction -= hist / 8000;
          d -= reduction;
          d = (d < 1) ? 1 : d;
        }

        score = -search(ply+1, d, -alpha-1, -alpha);

        if (!tc->finished && score > alpha) {
          score = -search(ply+1, new_depth, -beta, -alpha);
        }
      }
    }
    else { 

      int d = new_depth;

      if (depth >= 3 && played >= 2 && is_quiet && !in_check) {
        int reduction = lmr[depth < MAX_PLY ? depth : MAX_PLY - 1][played] + 1;
        reduction -= hist / 8000;
        d -= reduction;
        d = (d < 1) ? 1 : d;
      }

      score = -search(ply+1, d, -beta, -alpha);

      if (!tc->finished && score > alpha && d < new_depth) {
        score = -search(ply+1, new_depth, -beta, -alpha);
      }
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
          
          // =========================================================
          // PAWNOCCHIO IDEA #1: STATSCORE HISTORY
          // Scale the bonus dynamically based on how much the move 
          // beat the static evaluation of the position!
          // =========================================================
          int stat_diff = score - ev;
          if (stat_diff < 0) stat_diff = 0;
          
          int bonus = (18 * depth * depth) + (stat_diff * depth / 4);
          if (bonus > 2020)
            bonus = 2020;
          // =========================================================
          
          for (int i=0; i < played; i++) {
            const move_t pm = node->played[i];
            if (pm & MOVE_FLAG_CAPTURE)
              update_capture_history(pos, pm, pm == best_move ? bonus : -bonus);
          }
          if (!(best_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE))) {
            update_killer(node, best_move);
            update_piece_to_history(pos, best_move, bonus);
            update_cont_history(node, pos, best_move, bonus);
            
            // Update Countermove Table on cutoff
            if (ply > 0 && node->prev_piece != EMPTY) {
              counter_moves[node->prev_piece][node->prev_to] = best_move;
            }

            for (int i=0; i < played-1; i++) {
              const move_t pm = node->played[i];
              if (!(pm & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE))) {
                update_piece_to_history(pos, pm, -bonus);
                update_cont_history(node, pos, pm, -bonus);
              }
            }
          }
          
          if (!excluded && !in_check && best_score < MATEISH && best_score > ev && !(best_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE)))
            update_corrhist(pos, depth, best_score - ev);
          if (!excluded)
            tt_put(pos->hash, TT_BETA, depth, put_adjusted_score(ply, best_score), raw_ev, best_move, is_pv);
          return score;
        }
      }
    }
  }

  if (played == 0) {
    return in_check ? (-MATE + ply) : 0; 
  }

  if (!excluded && !in_check && best_score > -MATEISH && best_score < MATEISH && !(best_move & (MOVE_FLAG_CAPTURE | MOVE_FLAG_PROMOTE)) && !(alpha == orig_alpha && best_score >= ev))
    update_corrhist(pos, depth, best_score - ev);

  if (!excluded)
    tt_put(pos->hash, (alpha > orig_alpha) ? TT_EXACT : TT_ALPHA, depth, put_adjusted_score(ply, best_score), raw_ev, best_move, is_pv);

  return best_score;

}