#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include "timecontrol.h"
#include "search.h"
#include "go.h"
#include "hh.h"
#include "history.h"
#include "report.h"
#include "nodes.h"
#include "uci.h"
#include "pos.h"
#include "net.h"

extern int num_threads;

Position root_pos;
static int go_silent = 0;

void* search_worker(void* arg) {
  int thread_id = (int)(intptr_t)arg;
  TimeControl *tc = &time_control;
  int alpha = 0, beta = 0, delta = 0, score = 0;

  current_thread_id = thread_id;
  reset_local_nodes();
  hh_init_thread();

  clear_nodes();
  pos_copy(&root_pos, &nodes[0].pos);

  net_init_thread();
  net_slow_rebuild_accs(&nodes[0]);
  nodes[0].accs_dirty = 0;

  for (int depth = 1; depth <= tc->max_depth; depth++) {
    alpha = -INF;
    beta  = INF;
    delta = 10;

    if (depth >= 4) {
      alpha = score - delta;
      beta  = score + delta;
    }

    while (1) {
      score = search(0, depth, alpha, beta);

      if (tc->finished) break;

      if (score <= alpha) alpha = score - delta;
      else if (score >= beta) beta = score + delta;
      else break;

      delta += delta;
    }

    flush_local_nodes();

    if (thread_id == 0 && !tc->finished && !go_silent) {
      report(depth);
    }

    if (tc->finished) break;

    check_tc_nodes();
    if (tc->finished) break;
  }

  flush_local_nodes();
  return NULL;
}

void go(int silent) {
  pthread_t threads[256];
  time_control.finished = 0;
  go_silent = silent;
  hh_set_root();
  pos_copy(&nodes[0].pos, &root_pos);

  for (int i = 1; i < num_threads; i++) {
    pthread_create(&threads[i], NULL, search_worker, (void*)(intptr_t)i);
  }

  search_worker((void*)(intptr_t)0);

  for (int i = 1; i < num_threads; i++) {
    pthread_join(threads[i], NULL);
  }

  if (!silent) {
    char bm_str[6];
    format_move(time_control.best_move, bm_str);
    printf("bestmove %s\n", bm_str);
    fflush(stdout);
  }
}