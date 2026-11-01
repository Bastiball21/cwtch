#ifndef TT_H
#define TT_H

#include <stddef.h>
#include <stdint.h>
#include "move.h"
#include "pos.h"

// Bound is a bitmask: an exact score is both an upper and a lower bound.
#define TT_NONE 0
#define TT_ALPHA 1  // upper bound (failed low)
#define TT_BETA 2   // lower bound (failed high)
#define TT_EXACT 3

#define TT_DEFAULT_MB 256
#define TT_MAX_MB 262144
#define TT_NO_EVAL (-32768)

typedef struct {

  move_t move;
  int score;
  int eval;   // raw net output, TT_NO_EVAL if none
  int depth;
  int bound;
  int pv;

} TTHit;

int new_tt(size_t megabytes);
void tt_clear(void);
int is_tt_null(void);
void new_game(void);
void tt_new_search(void);
int tt_hashfull(void);
int tt_probe(const uint64_t hash, TTHit *hit);
void tt_prefetch(const uint64_t hash);
void tt_put(const uint64_t hash, const int bound, const int depth, const int score, const int eval, const move_t move, const int pv);
int get_adjusted_score(const int ply, const int score);
int put_adjusted_score(const int ply, const int score);

#endif
