#include <stdint.h>
#include <string.h>
#include "types.h"
#include "pos.h"
#include "corrhist.h"
#include "tune.h"

// The net has blind spots tied to particular structures. Each table remembers how
// far search results landed from the static eval for one feature of the position,
// and the eval is nudged by the weighted sum of what they remember.

static int16_t pawn_corr[2][CORR_SIZE];
static int16_t major_corr[2][CORR_SIZE];
static int16_t nonpawn_corr[2][2][CORR_SIZE];  // [stm][colour]
static int16_t cont_corr[2][12][64];           // [stm][previous piece][previous to]

void clear_corrhist(void) {
  memset(pawn_corr, 0, sizeof(pawn_corr));
  memset(major_corr, 0, sizeof(major_corr));
  memset(nonpawn_corr, 0, sizeof(nonpawn_corr));
  memset(cont_corr, 0, sizeof(cont_corr));
}

// splitmix64 finaliser
static inline uint64_t mix64(uint64_t x) {

  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;

  return x;

}

static inline uint64_t key2(const uint64_t a, const uint64_t b) {
  return mix64(a ^ mix64(b));
}

typedef struct {
  int16_t *pawn;
  int16_t *major;
  int16_t *nonpawn[2];
  int16_t *cont;  // NULL at the root and after a null move
} CorrEntries;

static inline void find_entries(const Node *node, CorrEntries *e) {

  const Position *pos = &node->pos;
  const uint64_t *a = pos->all;
  const int stm = pos->stm;

  const uint64_t wk = a[WKING], bk = a[BKING];
  const uint64_t majors = a[WROOK] | a[WQUEEN] | a[BROOK] | a[BQUEEN];

  e->pawn = &pawn_corr[stm][key2(a[WPAWN], a[BPAWN]) & (CORR_SIZE - 1)];
  e->major = &major_corr[stm][key2(majors & pos->colour[WHITE], majors & pos->colour[BLACK]) & (CORR_SIZE - 1)];
  e->nonpawn[WHITE] = &nonpawn_corr[stm][WHITE][mix64((pos->colour[WHITE] & ~a[WPAWN]) ^ wk) & (CORR_SIZE - 1)];
  e->nonpawn[BLACK] = &nonpawn_corr[stm][BLACK][mix64((pos->colour[BLACK] & ~a[BPAWN]) ^ bk) & (CORR_SIZE - 1)];
  e->cont = (node->prev_piece != EMPTY && node->cont_entry) ? &cont_corr[stm][node->prev_piece][node->prev_to] : NULL;

}

int correct_eval(const Node *node, const int ev) {

  CorrEntries e;
  find_entries(node, &e);

  // weights out of 256
  const int weighted = corr_pawn * *e.pawn
                     + corr_major * *e.major
                     + corr_nonpawn * (*e.nonpawn[WHITE] + *e.nonpawn[BLACK])
                     + (e.cont ? corr_cont * *e.cont : 0);

  // scores drift towards a draw as the 50 move limit nears
  const int faded = ev * (200 - node->pos.hmc) / 200;
  const int corrected = faded + weighted / (256 * CORR_GRAIN);

  if (corrected <= -MATEISH)
    return -MATEISH + 1;
  if (corrected >= MATEISH)
    return MATEISH - 1;

  return corrected;

}

static inline void update_entry(int16_t *entry, const int w, const int diff) {

  int v = (*entry * (CORR_WEIGHT_SCALE - w) + diff * CORR_GRAIN * w) / CORR_WEIGHT_SCALE;

  if (v < -CORR_LIMIT)
    v = -CORR_LIMIT;
  if (v > CORR_LIMIT)
    v = CORR_LIMIT;

  *entry = (int16_t)v;

}

void update_corrhist(const Node *node, const int depth, const int diff) {

  CorrEntries e;
  find_entries(node, &e);

  const int w = depth + 1 < CORR_WEIGHT_MAX ? depth + 1 : CORR_WEIGHT_MAX;

  update_entry(e.pawn, w, diff);
  update_entry(e.major, w, diff);
  update_entry(e.nonpawn[WHITE], w, diff);
  update_entry(e.nonpawn[BLACK], w, diff);
  if (e.cont)
    update_entry(e.cont, w, diff);

}
