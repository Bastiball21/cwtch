#ifndef HISTORY_H
#define HISTORY_H

#include <string.h>
#include "types.h"
#include "pos.h"
#include "move.h"
#include "nodes.h"

#define MAX_HISTORY 32766

extern int16_t main_history[2][64][64];       // [stm][from][to]
extern int16_t piece_to_history[12][64];
extern int16_t cont_history[12][64][12][64];  // [prev piece][prev to][piece][to]
extern int16_t capture_history[12][64][6];  // [piece][to][captured type] (ep/promo capture -> pawn)

static inline void clear_piece_to_history(void) {
  memset(main_history, 0, sizeof(main_history));
  memset(piece_to_history, 0, sizeof(piece_to_history));
}

static inline void clear_cont_history(void) {
  memset(cont_history, 0, sizeof(cont_history));
}

static inline void clear_capture_history(void) {
  memset(capture_history, 0, sizeof(capture_history));
}

extern move_t counter_moves[12][64];
static inline void clear_counter_moves(void) {
  memset(counter_moves, 0, sizeof(counter_moves));
}

// How well a quiet move has done: its own record plus its record after the moves
// played 1, 2 and 4 plies ago.
static inline int quiet_history(const Node *node, const int stm, const int piece, const int from, const int to) {
  int score = main_history[stm][from][to] + piece_to_history[piece][to];
  if (node->cont_entry)
    score += node->cont_entry[piece][to];
  if (node->cont2)
    score += node->cont2[piece][to];
  if (node->cont4)
    score += node->cont4[piece][to];
  return score;
}

void update_quiet_histories(Node *node, const Position *pos, const move_t move, int bonus);
void update_capture_history(const Position *pos, const move_t move, int bonus);
void update_killer(Node *node, const move_t move);

#endif