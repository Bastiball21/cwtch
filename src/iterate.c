#include <stdint.h>
#include "iterate.h"
#include "move.h"
#include "nodes.h"
#include "pos.h"
#include "movegen.h"
#include "history.h"
#include "see.h"

enum {
  STAGE_TT,
  STAGE_GEN_NOISY,
  STAGE_GOOD_NOISY,
  STAGE_GEN_QUIET,
  STAGE_QUIET,
  STAGE_BAD_NOISY,
  STAGE_DONE
};

// Far above any history sum, so these two are tried first among the quiets.
#define KILLER_RANK (1 << 20)
#define KILLER2_RANK (1 << 19)
#define COUNTERMOVE_RANK (1 << 18)

static inline int is_underpromotion(const move_t move) {
  return (move & MOVE_FLAG_PROMOTE) && ((move >> 12) & 7) != QUEEN;
}

static void remove_tt_move(Node *node) {
  const move_t tt_move = node->tt_move;
  if (tt_move == 0)
    return;

  move_t *moves = node->moves;
  const int n = node->num_moves;

  for (int i = 0; i < n; i++) {
    if (moves[i] == tt_move) {
      moves[i] = moves[n - 1];
      node->num_moves--;
      return;
    }
  }
}

static move_t get_next_sorted_move(Node *const node) {
  move_t max_m  = 0;
  move_t *moves = node->moves;
  int32_t *ranks = node->ranks;
  const int next = node->next_move;
  const int num = node->num_moves;
  int32_t max_r = INT32_MIN;
  int max_i = next;

  for (int i=next; i < num; i++) {
    if (ranks[i] > max_r) {
      max_r = ranks[i];
      max_i = i;
    }
  }

  max_m = moves[max_i];
  moves[max_i] = moves[next];
  ranks[max_i] = ranks[next];

  node->next_move++;
  return max_m;
}

static void rank_quiets(Node *node) {
  const uint8_t *board = node->pos.board;
  const move_t *moves = node->moves;
  const move_t killer = node->killer;
  const move_t killer2 = node->killer2;
  int32_t *ranks = node->ranks;
  const int stm = node->pos.stm;
  const int n = node->num_moves;

  move_t countermove = 0;
  if (node->prev_piece != EMPTY) {
    countermove = counter_moves[node->prev_piece][node->prev_to];
  }

  for (int i=0; i < n; i++) {
    const move_t m = moves[i];
    const int from = (m >> 6) & 0x3F;
    const int to = m & 0x3F;

    ranks[i] = quiet_history(node, stm, board[from], from, to);

    if (m == killer)
      ranks[i] += KILLER_RANK;
    else if (m == killer2)
      ranks[i] += KILLER2_RANK;
    else if (m == countermove)
      ranks[i] += COUNTERMOVE_RANK;
  }
}

static const int32_t mvv_augment[6] = {0, 4800, 4800, 9600, 19200, 0};

static void rank_noisy(Node *node) {
  const uint8_t *board = node->pos.board;
  const move_t *moves = node->moves;
  int32_t *ranks = node->ranks;
  const int n = node->num_moves;

  for (int i=0; i < n; i++) {
    const move_t m = moves[i];
    const int from = (m >> 6) & 0x3F;
    const int to = m & 0x3F;

    if (m & MOVE_FLAG_CAPTURE) {
      const int piece = board[from];
      int victim = board[to];
      victim = (victim == EMPTY) ? PAWN : victim % 6;

      ranks[i] = 64000 + mvv_augment[victim] + (5 - piece % 6) + capture_history[piece][to][victim];
      if (m & MOVE_FLAG_PROMOTE)
        ranks[i] += mvv_augment[(m >> 12) & 0x7];
    }
    else {
      const int promo_piece = (m >> 12) & 0x7;
      ranks[i] = 64000 + mvv_augment[promo_piece];
    }
  }
}

static void start_noisy(Node *node) {
  node->num_moves = 0;
  node->next_move = 0;
  node->num_bad = 0;
  node->next_bad = 0;
  gen_noisy(node);
  remove_tt_move(node);
  rank_noisy(node);
}

void init_next_search_move(Node *node, const int in_check, const move_t tt_move) {
  node->stage = STAGE_TT;
  node->in_check = in_check;
  node->tt_move = tt_move;
  node->skip_quiets = 0;
}

// The singular search reuses this node's move list: start again just behind the
// TT move, which the caller has already taken.
void restart_after_tt_move(Node *node, const move_t tt_move) {
  node->stage = STAGE_GEN_NOISY;
  node->tt_move = tt_move;
  node->skip_quiets = 0;
}

move_t get_next_search_move(Node *node) {
  switch (node->stage) {
    case STAGE_TT:
      node->stage = STAGE_GEN_NOISY;
      if (node->tt_move)
        return node->tt_move;
      /* fall through */

    case STAGE_GEN_NOISY:
      node->stage = STAGE_GOOD_NOISY;
      start_noisy(node);
      /* fall through */

    case STAGE_GOOD_NOISY:
      while (node->next_move < node->num_moves) {
        const move_t move = get_next_sorted_move(node);
        if (is_underpromotion(move) || !see_ge(&node->pos, move, 0)) {
          node->bad_noisy[node->num_bad++] = move;
          continue;
        }
        return move;
      }
      node->stage = STAGE_GEN_QUIET;
      /* fall through */

    case STAGE_GEN_QUIET:
      node->stage = STAGE_QUIET;
      node->num_moves = 0;
      node->next_move = 0;
      if (!node->skip_quiets) {
        gen_quiets(node);
        remove_tt_move(node);
        rank_quiets(node);
      }
      /* fall through */

    case STAGE_QUIET:
      if (!node->skip_quiets && node->next_move < node->num_moves)
        return get_next_sorted_move(node);
      node->stage = STAGE_BAD_NOISY;
      /* fall through */

    case STAGE_BAD_NOISY:
      if (node->next_bad < node->num_bad)
        return node->bad_noisy[node->next_bad++];
      node->stage = STAGE_DONE;
      /* fall through */

    default:
      return 0;
  }
}

int is_bad_noisy_stage(const Node *node) {
  return node->stage >= STAGE_BAD_NOISY;
}

void init_next_qsearch_move(Node *node, const int in_check, const move_t tt_move) {
  init_next_search_move(node, in_check, tt_move);
}

// In check every evasion matters, so the full move list is used.
move_t get_next_qsearch_move(Node *node) {

  if (node->in_check)
    return get_next_search_move(node);

  switch (node->stage) {
    case STAGE_TT:
      node->stage = STAGE_GEN_NOISY;
      if (node->tt_move)
        return node->tt_move;
      /* fall through */

    case STAGE_GEN_NOISY:
      node->stage = STAGE_GOOD_NOISY;
      start_noisy(node);
      /* fall through */

    case STAGE_GOOD_NOISY:
      while (node->next_move < node->num_moves) {
        const move_t move = get_next_sorted_move(node);
        if (!is_underpromotion(move))
          return move;
      }
      node->stage = STAGE_DONE;
      /* fall through */

    default:
      return 0;
  }
}
