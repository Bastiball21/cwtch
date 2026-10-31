#ifndef POS_H
#define POS_H

#include <stdint.h>
#include "types.h"

#define EMPTY 255

#define WHITE_RIGHTS_KING  1
#define WHITE_RIGHTS_QUEEN 2
#define BLACK_RIGHTS_KING  4
#define BLACK_RIGHTS_QUEEN 8
#define ALL_RIGHTS         15

typedef struct {

  uint64_t all[12];
  uint64_t colour[2];
  uint64_t occupied;

  uint8_t board[64];

  uint8_t stm;
  uint8_t rights;
  uint8_t ep;
  uint8_t hmc;
  uint64_t hash;
  // New fields for Chess960
  int is_chess960; 
  uint8_t castling_rook_sq[2][2]; // [colour][kingside=0, queenside=1]

} Position;

static inline void pos_copy(const Position *from_pos, Position *to_pos) {
  *to_pos = *from_pos;
}

static inline int is_pawn_endgame(const Position *pos) {
  const uint64_t *const a = pos->all;
  return pos->occupied == (a[WKING] | a[WPAWN] | a[BKING] | a[BPAWN]);
}

void print_board(const Position *pos);
int is_attacked(const Position *pos, const int sq, const int opp);
int is_mat_draw(const Position *pos);

#endif