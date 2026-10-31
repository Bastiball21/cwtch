#include <stdio.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include "builtins.h"
#include "timecontrol.h"
#include "search.h"
#include "go.h"
#include "hh.h"
#include "history.h"
#include "makemove.h"
#include "report.h"
#include "nodes.h"
#include "tt.h"
#include "uci.h"
#include "pos.h"
#include "net.h"

extern int num_threads;

Position root_pos;
static int go_silent = 0;

static void sleep_ms(const long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

void* search_worker(void* arg) {
  int thread_id = (int)(intptr_t)arg;
  TimeControl *tc = &time_control;
  int alpha = 0, beta = 0, delta = 0, score = 0;

  search_thread_init(thread_id);
  hh_init_thread();

  clear_nodes();
  pos_copy(&root_pos, &nodes[0].pos);

  net_init_thread();
  net_slow_rebuild_accs(&nodes[0]);
  nodes[0].accs_dirty = 0;

  for (int depth = 1; depth <= tc->max_depth; depth++) {
    alpha = -INF;
    beta  = INF;
    delta = 12 + (score * score) / 16384;

    if (depth >= 4) {
      alpha = score - delta > -INF ? score - delta : -INF;
      beta  = score + delta < INF ? score + delta : INF;
    }

    while (1) {
      score = search(0, depth, alpha, beta, 0);

      if (tc->finished) break;

      if (score <= alpha) {
        beta = (alpha + beta) / 2;
        alpha = score - delta > -INF ? score - delta : -INF;
      }
      else if (score >= beta) {
        beta = score + delta < INF ? score + delta : INF;
      }
      else {
        break;
      }

      delta += delta / 2;
    }

    flush_local_nodes();

    if (thread_id == 0 && !tc->finished && !go_silent) {
      report(depth, score, sel_depth);
    }

    if (tc->finished) break;

    check_tc_nodes();
    if (tc->finished) break;

    if (thread_id == 0) {

      tc->best_score = score;

      // no best move from a finished iteration means mate or stalemate at the root
      if (!tc->best_move)
        break;

      if (tc_iteration_done(depth, tc->best_move, score, root_move_nodes(tc->best_move), thread_nodes))
        break;

    }
  }

  flush_local_nodes();

  if (thread_id == 0) {

    // `go infinite` may not answer until told to stop, however little there is to search
    while (tc->infinite && !tc->finished)
      sleep_ms(1);

    tc->finished = 1;  // helpers stop with the main thread

  }

  return NULL;
}

void go(int silent) {
  pthread_t threads[256];
  int started[256] = {0};
  go_silent = silent;
  hh_set_root();
  tt_new_search();
  pos_copy(&nodes[0].pos, &root_pos);

  for (int i = 1; i < num_threads; i++) {
    started[i] = pthread_create(&threads[i], NULL, search_worker, (void*)(intptr_t)i) == 0;
  }

  search_worker((void*)(intptr_t)0);

  for (int i = 1; i < num_threads; i++) {
    if (started[i])
      pthread_join(threads[i], NULL);
  }

  // a search can be stopped before it finishes looking at its first move
  if (!time_control.best_move)
    time_control.best_move = first_legal_move(&nodes[0]);

  if (!silent) {
    if (time_control.best_move) {
      char bm_str[6];
      format_move(time_control.best_move, bm_str);
      printf("bestmove %s\n", bm_str);
    }
    else {
      printf("bestmove 0000\n");
    }
    fflush(stdout);
  }
}
