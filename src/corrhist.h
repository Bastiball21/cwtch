#ifndef CORRHIST_H
#define CORRHIST_H

#include <string.h>
#include <stdint.h>
#include "types.h"
#include "pos.h"

#define CORR_SIZE 16384  // power of two
#define CORR_GRAIN 256   // entry units per cp
#define CORR_LIMIT (32 * CORR_GRAIN)
#define CORR_WEIGHT_MAX 16
#define CORR_WEIGHT_SCALE 256

extern int16_t pawn_corr[2][CORR_SIZE];

static inline void clear_corrhist(void) {
  memset(pawn_corr, 0, sizeof(pawn_corr));
}

int correct_eval(const Position *pos, const int ev);
void update_corrhist(const Position *pos, const int depth, const int diff);

#endif
