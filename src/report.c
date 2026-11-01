#include <stdio.h>
#include "types.h"
#include "timecontrol.h"
#include "pv.h"
#include "tt.h"

void report(int depth, int score, int sel_depth) {

  const int pvl = pv_len[0];
  int next_pv_char = 0;
  move_t *const pv = pv_table[0];
  char pv_str[MAX_PLY * 6 + 1];
  char score_str[32];
  TimeControl *tc = &time_control;

  pv_str[0] = '\0';

  for (int i=pvl-1; i >= 0; i--) {
    next_pv_char += format_move(pv[i], &pv_str[next_pv_char]);
    pv_str[next_pv_char++] = ' ';
  }

  pv_str[next_pv_char] = '\0';

  if (score > MATEISH)
    snprintf(score_str, sizeof(score_str), "mate %d", (MATE - score + 1) / 2);
  else if (score < -MATEISH)
    snprintf(score_str, sizeof(score_str), "mate %d", -(MATE + score + 1) / 2);
  else
    snprintf(score_str, sizeof(score_str), "cp %d", score);

  const unsigned long long elapsed_ms = time_ms() - tc->start_time;
  const unsigned long long total_nodes = tc->nodes;
  const unsigned long long nps = (total_nodes * 1000ULL) / (elapsed_ms ? elapsed_ms : 1);

  printf("info depth %d seldepth %d score %s time %llu nodes %llu nps %llu hashfull %d pv %s\n",
         depth, sel_depth, score_str, elapsed_ms, total_nodes, nps, tt_hashfull(), pv_str);

}
