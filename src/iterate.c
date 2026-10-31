#include <stdint.h>
#include "iterate.h"
#include "move.h"
#include "nodes.h"
#include "pos.h"
#include "movegen.h"
#include "history.h"

#define COUNTERMOVE 32766

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
  int max_i;

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
  int32_t *ranks = node->ranks;
  int16_t (*const cont)[64] = node->cont_entry;
  const int n = node->num_moves;

  move_t countermove = 0;
  if (node->prev_piece != EMPTY) {
    countermove = counter_moves[node->prev_piece][node->prev_to];
  }

  for (int i=0; i < n; i++) {
    const move_t m = moves[i];

    if (m == killer) {
      ranks[i] = KILLER;
    }
    else if (m == countermove && countermove != 0) {
      ranks[i] = COUNTERMOVE;
    }
    else {
      const int from = (m >> 6) & 0x3F;
      const int to = m & 0x3F;
      const int piece = board[from];
      ranks[i] = cont ? (piece_to_history[piece][to] + cont[piece][to]) / 2 : piece_to_history[piece][to];
    }
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
    }
    else {
      const int promo_piece = (m >> 12) & 0x7;
      ranks[i] = 64000 + mvv_augment[promo_piece];
    }
  }
}

void init_next_search_move(Node *node, const int in_check, const move_t tt_move) {
  node->stage = 0;
  node->in_check = in_check;
  node->tt_move = tt_move;
}

move_t get_next_search_move(Node *node) {
  switch (node->stage) {
    case 0:
      node->stage++;
      node->num_moves = 0;
      node->next_move = 0;
      if (node->tt_move)
        return node->tt_move;
      /* fall through */

    case 1:
      node->stage++;
      node->num_moves = 0;
      node->next_move = 0;
      gen_noisy(node);
      remove_tt_move(node);
      rank_noisy(node);
      /* fall through */

    case 2:
      if (node->next_move < node->num_moves)
        return get_next_sorted_move(node);
      node->stage++;
      node->num_moves = 0;
      node->next_move = 0;
      gen_quiets(node);
      remove_tt_move(node);
      rank_quiets(node);
      /* fall through */

    case 3:
      if (node->next_move < node->num_moves)
        return get_next_sorted_move(node);
      return 0;

    default:
      return 0;
  }
}

void init_next_qsearch_move(Node *node, const int in_check, const move_t tt_move) {
  node->stage = 0;
  node->in_check = in_check;
  node->tt_move = tt_move;
}

move_t get_next_qsearch_move(Node *node) {
  switch (node->stage) {
    case 0:
      node->stage++;
      node->num_moves = 0;
      node->next_move = 0;
      if (node->tt_move)
        return node->tt_move;
      /* fall through */

    case 1:
      node->stage++;
      node->num_moves = 0;
      node->next_move = 0;
      gen_noisy(node);
      remove_tt_move(node);
      rank_noisy(node);
      /* fall through */

    case 2:
      if (node->next_move < node->num_moves)
        return get_next_sorted_move(node);
      return 0;

    default:
      return 0;
  }
}