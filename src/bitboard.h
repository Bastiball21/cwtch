#ifndef BITBOARD_H
#define BITBOARD_H

#include <stdint.h>

typedef struct {

  int bits;
  int count;
  int shift;
  uint64_t mask;
  uint64_t magic;
  uint64_t *attacks;

} Attack;

extern uint64_t pawn_attacks[2][64];
extern uint64_t knight_attacks[64];
extern Attack bishop_attacks[64];
extern Attack rook_attacks[64];
extern uint64_t king_attacks[64];
extern uint64_t all_attacks[64];
extern uint64_t all_attacks_inc_edge[64];

static inline int magic_index(const uint64_t blockers, const uint64_t magic, const int shift) {
  return (int)((blockers * magic) >> shift);
}

void init_attacks(void);

#endif
