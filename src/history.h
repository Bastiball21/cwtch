#ifndef HISTORY_H
#define HISTORY_H

#include <string.h>
#include "types.h"
#include "pos.h"
#include "move.h"
#include "nodes.h"

#define MAX_HISTORY 32766
#define KILLER 32767

extern int16_t piece_to_history[12][64];
extern int16_t cont_history[12][64][12][64];  // [prev piece][prev to][piece][to]
extern int16_t capture_history[12][64][6];  // [piece][to][captured type] (ep/promo capture -> pawn)

static inline void clear_piece_to_history(void) {
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

void update_piece_to_history(const Position *pos, const move_t move, int bonus);
void update_cont_history(Node *node, const Position *pos, const move_t move, int bonus);
void update_capture_history(const Position *pos, const move_t move, int bonus);
void update_killer(Node *node, const move_t move);

#endif