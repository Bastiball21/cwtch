#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>
#include "types.h"

#define move_t uint32_t

#define MOVE_FLAG_PROMOTE_Q (QUEEN << 12)
#define MOVE_FLAG_PROMOTE_R (ROOK << 12)
#define MOVE_FLAG_PROMOTE_B (BISHOP << 12)
#define MOVE_FLAG_PROMOTE_N (KNIGHT << 12)
#define MOVE_FLAG_PROMOTE (0x8 << 12)
#define MOVE_FLAG_CAPTURE (0x10 << 12)
#define MOVE_FLAG_EPCAPTURE (0x20 << 12)
#define MOVE_FLAG_PAWN2 (0x40 << 12)
#define MOVE_FLAG_CASTLE (0x80 << 12)
#define MOVE_FLAGS_EXTRA (MOVE_FLAG_CAPTURE | MOVE_FLAG_EPCAPTURE | MOVE_FLAG_PROMOTE | MOVE_FLAG_PAWN2 | MOVE_FLAG_CASTLE)

static inline move_t encode_move(const int from, const int to, const int flags) {
  return (from << 6) | to | flags;
}

int format_move(const move_t move, char *const buf);

#endif
