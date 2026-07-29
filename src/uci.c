#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "types.h"
#include "uci.h"
#include "nodes.h"
#include "pos.h"
#include "position.h"
#include "perft.h"
#include "timecontrol.h"
#include "go.h"
#include "evaluate.h"
#include "net.h"
#include "bench.h"
#include "tt.h"
#include "input.h"
#include "datagen.h"

#define MAX_TOKENS 1024

int num_threads = 1;
int is_chess960 = 0;

static bool str_eq(const char *a, const char *b, const char *c) {
  return (strcmp(a, b) == 0) || (strcmp(a, c) == 0);
}

static int tokenize(char *input, char *tokens[], int max_tokens) {
  int count = 0;
  char *token = strtok(input, " \t");
  while (token && count < max_tokens) {
    tokens[count++] = token;
    token = strtok(NULL, " \t");
  }
  return count;
}

bool uci_exec(char *input) {

  char *tokens[MAX_TOKENS];
  int ntokens = tokenize(input, tokens, MAX_TOKENS);

  if (ntokens == 0)
    return true;

  const char *cmd = tokens[0];

  if (str_eq(cmd, "uci", "")) {
    printf("id name %s %s\n", "Cwtch", BUILD);
    printf("id author Colin Jenkins & Basti Dangca\n");
    printf("option name Hash type spin default %d min 1 max 262144\n", TT_DEFAULT_MB);
    printf("option name Threads type spin default 1 min 1 max 256\n");
    printf("option name UCI_Chess960 type check default false\n");
    printf("option name LoadNet type string default\n");
    printf("uciok\n");
  }
  
  else if (str_eq(cmd, "isready", "")) {
    printf("readyok\n");
  }
  
  else if (str_eq(cmd, "ucinewgame", "u")){
    new_game();
    position(&nodes[0], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", 0, 0, NULL);
  }

  else if (str_eq(cmd, "setoption", "")) {
    if (strcasecmp(tokens[2], "Hash") == 0) {
      int mb = atoi(tokens[4]);
      new_tt(mb);
    }
    else if (strcasecmp(tokens[2], "Threads") == 0) {
      num_threads = atoi(tokens[4]);
      if (num_threads < 1) num_threads = 1;
      if (num_threads > 256) num_threads = 256; // Safeguard limit
    }
    else if (strcasecmp(tokens[2], "LoadNet") == 0) {
      if (ntokens >= 5) {
        load_weights_from_file(tokens[4]);
      }
    }
    else if (strcasecmp(tokens[2], "UCI_Chess960") == 0) {
      is_chess960 = (strcasecmp(tokens[4], "true") == 0);
    }
  }

  else if (str_eq(cmd, "position", "p")) {

    const char *fen_option = tokens[1];
    if (str_eq(fen_option, "startpos", "s")) {
      int num_moves = 0;
      char **moves_ptr = NULL;
      if (ntokens > 3) {
        num_moves = ntokens - 3;
        moves_ptr = &tokens[3];
      }
      position(&nodes[0], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", 0, num_moves, moves_ptr);
    }
    else if (str_eq(fen_option, "fen", "f")) {
      int num_moves = 0;
      char **moves_ptr = NULL;
      int hmc = atoi(tokens[6]);
      if (ntokens > 9) {
        num_moves = ntokens - 9;
        moves_ptr = &tokens[9];
      }
      position(&nodes[0], tokens[2], tokens[3], tokens[4], tokens[5], hmc, num_moves, moves_ptr);
    }
  }
  
  else if (str_eq(cmd, "go", "g")) {

    if (is_tt_null())
      new_tt(TT_DEFAULT_MB);


    int64_t wtime = 0;
    int64_t winc = 0;
    int64_t btime = 0;
    int64_t binc = 0;
    int64_t max_nodes = 0;
    int64_t move_time = 0;
    int max_depth = 0;
    int moves_to_go = 0;
    
    int t = 1;
    
    while (t < ntokens) {

      const char *token = tokens[t];

      if (str_eq(token, "wtime", "") && t + 1 < ntokens) {
        t++;
        wtime = atoi(tokens[t]);
      }
      else if (str_eq(token, "winc", "") && t + 1 < ntokens) {
        t++;
        winc = atoi(tokens[t]);
      }
      else if (str_eq(token, "btime", "") && t + 1 < ntokens) {
        t++;
        btime = atoi(tokens[t]);
      }
      else if (str_eq(token, "binc", "") && t + 1 < ntokens) {
        t++;
        binc = atoi(tokens[t]);
      }
      else if (str_eq(token, "depth", "d") && t + 1 < ntokens) {
        t++;
        max_depth = atoi(tokens[t]);
      }
      else if (str_eq(token, "infinite", "i")) {
        max_depth = MAX_PLY - 1;
      }
      else if (str_eq(token, "nodes", "n") && t + 1 < ntokens) {
        t++;
        max_nodes = atoi(tokens[t]);
      }
      else if (str_eq(token, "movetime", "m") && t + 1 < ntokens) {
        t++;
        move_time = atoi(tokens[t]);
      }
      else if (str_eq(token, "movestogo", "") && t + 1 < ntokens) {
        t++;
        moves_to_go = atoi(tokens[t]);
      }

      t++;

    }
    
    init_tc(wtime, winc, btime, binc, max_nodes, move_time, max_depth, moves_to_go);
    input_set_searching(1);
    go(0);
    input_set_searching(0);
  }
  
  else if (str_eq(cmd, "quit", "q")) {
    return false;
  }
  
  else if (str_eq(cmd, "board", "b")) {
    print_board(&nodes[0].pos);
  }
  
  else if (str_eq(cmd, "eval", "e")) {
    int16_t score = net_eval(&nodes[0]);
    printf("eval: %d cp (white POV)\n", score);
  }
  
  else if (str_eq(cmd, "perft", "f")) {
    int depth = (ntokens > 1) ? atoi(tokens[1]) : 1;
    clock_t start = clock();
    uint64_t num_nodes = perft(depth, 0);
    clock_t end = clock();
    double secs = (double)(end - start) / CLOCKS_PER_SEC;
    uint64_t nps = secs > 0 ? (uint64_t)(num_nodes / secs) : 0;
    printf("perft %d: %lu (%.3fs, %lu nps)\n", depth, num_nodes, secs, nps);
  }

  else if (str_eq(cmd, "pt", "")) {
    int max_depth = (ntokens > 1) ? atoi(tokens[1]) : 0;
    perft_tests(max_depth);
  }

  else if (str_eq(cmd, "et", "")) {
    eval_tests();
  }

  else if (str_eq(cmd, "net", "n")) {
    printf("net: %s\n", NET_WEIGHTS_PATH);
    printf("arch: NNUE (%dx%d%s->%d)x2->1x%d\n", NET_I_SIZE, NET_I_BUCKETS, NET_MIRRORED ? "hm" : "", NET_H1_SIZE, NET_O_BUCKETS);
    printf("act: squared relu\n");
    printf("quant: QA=%d QB=%d QAB=%d scale=%d\n", NET_QA, NET_QB, NET_QAB, NET_SCALE);
  }

  else if (str_eq(cmd, "loadnet", "ln")) {
    if (ntokens < 2) {
      printf("usage: loadnet <file>\n");
    }
    else {
      load_weights_from_file(tokens[1]);
    }
  }

  else if (str_eq(cmd, "bench", "h")) {
    int depth = 10;
    if (ntokens > 1)
      depth = atoi(tokens[1]);
    bench(depth);
  }

  else if (str_eq(cmd, "datagen", "dg")) {
    if (ntokens < 3) {
      printf("usage: datagen <directory> <positions>\n");
      return true;
    }
    datagen(tokens[1], (uint64_t)atof(tokens[2]));
  }

  else {
    printf("unknown command: %s\n", cmd);
  }

  return true;

}
