#ifndef PV_H
#define PV_H

#include <string.h>
#include "nodes.h"

extern _Thread_local move_t pv_table[MAX_PLY][MAX_PLY];
extern _Thread_local int pv_len[MAX_PLY];

static inline void collect_pv(const int ply, const move_t move) {

  const int next_ply = ply + 1;

  memcpy(pv_table[ply], pv_table[next_ply], (size_t)pv_len[next_ply] * sizeof(move_t));

  pv_len[ply]  = pv_len[next_ply];
  pv_table[ply][pv_len[ply]++] = move;

}

#endif
