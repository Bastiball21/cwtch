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

#define MAX_TOKENS 8192

static const char *const STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR";

int num_threads = 1;
int is_chess960 = 0;

static bool str_eq(const char *a, const char *b, const char *c) {
  return (strcmp(a, b) == 0) || (strcmp(a, c) == 0);
}

static int tokenize(char *input, char *tokens[], int max_tokens) {
  int count = 0;
  char *token = strtok(input, " \t\r\n");
  while (token && count < max_tokens) {
    tokens[count++] = token;
    token = strtok(NULL, " \t\r\n");
  }
  return count;
}

static int clamp_int(const long long v, const int lo, const int hi) {
  return v < lo ? lo : (v > hi ? hi : (int)v);
}

// setoption name <may contain spaces> [value <may contain spaces>]
static void uci_setoption(char *tokens[], const int ntokens) {

  char name[128] = "";
  char value[1024] = "";
  int i = 1;

  if (i < ntokens && strcmp(tokens[i], "name") == 0)
    i++;

  for (; i < ntokens && strcmp(tokens[i], "value") != 0; i++) {
    if (name[0])
      strncat(name, " ", sizeof(name) - strlen(name) - 1);
    strncat(name, tokens[i], sizeof(name) - strlen(name) - 1);
  }

  for (i++; i < ntokens; i++) {
    if (value[0])
      strncat(value, " ", sizeof(value) - strlen(value) - 1);
    strncat(value, tokens[i], sizeof(value) - strlen(value) - 1);
  }

  if (strcasecmp(name, "Clear Hash") == 0) {
    new_game();
    return;
  }

  if (!value[0]) {
    printf("info string setoption %s needs a value\n", name[0] ? name : "(no name)");
    return;
  }

  if (strcasecmp(name, "Hash") == 0) {
    new_tt((size_t)clamp_int(atoll(value), 1, TT_MAX_MB));
  }
  else if (strcasecmp(name, "Threads") == 0) {
    num_threads = clamp_int(atoll(value), 1, 256);
  }
  else if (strcasecmp(name, "Move Overhead") == 0) {
    move_overhead = clamp_int(atoll(value), 0, 5000);
  }
  else if (strcasecmp(name, "LoadNet") == 0) {
    load_weights_from_file(value);
  }
  else if (strcasecmp(name, "UCI_Chess960") == 0) {
    is_chess960 = (strcasecmp(value, "true") == 0);
  }
  else {
    printf("info string unknown option %s\n", name);
  }

}

// position startpos|fen <board> [stm] [castling] [ep] [hmc] [fullmove] [moves ...]
static void uci_position(char *tokens[], const int ntokens) {

  if (ntokens < 2) {
    printf("info string position needs startpos or fen\n");
    return;
  }

  int moves_at = ntokens;
  for (int i = 2; i < ntokens; i++) {
    if (strcmp(tokens[i], "moves") == 0) {
      moves_at = i;
      break;
    }
  }

  const int num_moves = moves_at + 1 < ntokens ? ntokens - moves_at - 1 : 0;
  char **moves_ptr = num_moves ? &tokens[moves_at + 1] : NULL;

  if (str_eq(tokens[1], "startpos", "s")) {
    position(&nodes[0], STARTPOS, "w", "KQkq", "-", 0, num_moves, moves_ptr);
    return;
  }

  if (!str_eq(tokens[1], "fen", "f") || moves_at < 3) {
    printf("info string position needs startpos or fen\n");
    return;
  }

  const char *board = tokens[2];

  if (!fen_board_valid(board)) {
    printf("info string invalid fen %s\n", board);
    return;
  }

  const int fields = moves_at - 2;
  const char *stm = fields > 1 ? tokens[3] : "w";
  const char *rights = fields > 2 ? tokens[4] : "-";
  const char *ep = fields > 3 ? tokens[5] : "-";
  const int hmc = fields > 4 ? atoi(tokens[6]) : 0;

  position(&nodes[0], board, stm, rights, ep, hmc, num_moves, moves_ptr);

}

bool uci_exec(char *input) {

  static char *tokens[MAX_TOKENS];
  int ntokens = tokenize(input, tokens, MAX_TOKENS);

  if (ntokens == 0)
    return true;

  const char *cmd = tokens[0];

  if (str_eq(cmd, "uci", "")) {
    printf("id name %s %s\n", "Cwtch", BUILD);
    printf("id author Colin Jenkins & Basti Dangca\n");
    printf("info string os %s | arch %s | cpu %s | simd %s\n",
           net_os_name(), net_arch_name(), net_cpu_brand(), net_simd_name());
    printf("option name Hash type spin default %d min 1 max %d\n", TT_DEFAULT_MB, TT_MAX_MB);
    printf("option name Threads type spin default 1 min 1 max 256\n");
    printf("option name Move Overhead type spin default 10 min 0 max 5000\n");
    printf("option name Clear Hash type button\n");
    printf("option name UCI_Chess960 type check default false\n");
    printf("option name LoadNet type string default\n");
    printf("uciok\n");
  }

  else if (str_eq(cmd, "isready", "")) {
    if (is_tt_null())
      new_tt(TT_DEFAULT_MB);
    printf("readyok\n");
  }

  else if (str_eq(cmd, "ucinewgame", "u")){
    new_game();
    position(&nodes[0], STARTPOS, "w", "KQkq", "-", 0, 0, NULL);
  }

  else if (str_eq(cmd, "setoption", "")) {
    uci_setoption(tokens, ntokens);
  }

  else if (str_eq(cmd, "position", "p")) {
    uci_position(tokens, ntokens);
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
    int infinite = 0;

    int t = 1;

    while (t < ntokens) {

      const char *token = tokens[t];

      if (str_eq(token, "wtime", "") && t + 1 < ntokens) {
        wtime = atoll(tokens[++t]);
      }
      else if (str_eq(token, "winc", "") && t + 1 < ntokens) {
        winc = atoll(tokens[++t]);
      }
      else if (str_eq(token, "btime", "") && t + 1 < ntokens) {
        btime = atoll(tokens[++t]);
      }
      else if (str_eq(token, "binc", "") && t + 1 < ntokens) {
        binc = atoll(tokens[++t]);
      }
      else if (str_eq(token, "depth", "d") && t + 1 < ntokens) {
        max_depth = clamp_int(atoll(tokens[++t]), 0, MAX_PLY - 1);
      }
      else if (str_eq(token, "infinite", "i")) {
        // only wait for stop when something can actually send it
        infinite = input_is_running();
        if (!infinite)
          max_depth = MAX_PLY - 1;
      }
      else if (str_eq(token, "nodes", "n") && t + 1 < ntokens) {
        max_nodes = atoll(tokens[++t]);
      }
      else if (str_eq(token, "movetime", "m") && t + 1 < ntokens) {
        move_time = atoll(tokens[++t]);
      }
      else if (str_eq(token, "movestogo", "") && t + 1 < ntokens) {
        moves_to_go = clamp_int(atoll(tokens[++t]), 0, 10000);
      }

      t++;

    }

    init_tc(wtime, winc, btime, binc, max_nodes, move_time, max_depth, moves_to_go, infinite);
    input_go_begin();
    go(0);
    input_go_end();
  }

  else if (str_eq(cmd, "quit", "q")) {
    return false;
  }
  
  else if (str_eq(cmd, "board", "b")) {
    print_board(&nodes[0].pos);
  }
  
  else if (str_eq(cmd, "eval", "e")) {
    int score = net_eval(&nodes[0]);
    printf("eval: %d cp (side to move)\n", score);
  }
  
  else if (str_eq(cmd, "perft", "f")) {
    int depth = (ntokens > 1) ? atoi(tokens[1]) : 1;
    clock_t start = clock();
    uint64_t num_nodes = perft(depth, 0);
    clock_t end = clock();
    double secs = (double)(end - start) / CLOCKS_PER_SEC;
    uint64_t nps = secs > 0 ? (uint64_t)(num_nodes / secs) : 0;
    printf("perft %d: %llu (%.3fs, %llu nps)\n", depth, (unsigned long long)num_nodes, secs, (unsigned long long)nps);
  }

  else if (str_eq(cmd, "pt", "")) {
    int max_depth = (ntokens > 1) ? atoi(tokens[1]) : 0;
    perft_tests(max_depth);
  }

  else if (str_eq(cmd, "et", "")) {
    eval_tests();
  }

  else if (str_eq(cmd, "net", "n")) {
    printf("os: %s (%s)\n", net_os_name(), net_arch_name());
    printf("cpu: %s\n", net_cpu_brand());
    printf("net: %s\n", NET_WEIGHTS_PATH);
    printf("arch: NNUE (%dx%d%s->%d)x2->1x%d\n", NET_I_SIZE, NET_I_BUCKETS, NET_MIRRORED ? "hm" : "", NET_H1_SIZE, NET_O_BUCKETS);
    printf("act: clipped squared relu (screlu)\n");
    printf("simd: %s (runtime-dispatched: avx512_vnni | avx512bw | avx2 | scalar)\n", net_simd_name());
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
      printf("usage: datagen <directory> <positions> [book_file|frc|dfrc]\n");
      return true;
    }

    const char *third_arg = (ntokens > 3) ? tokens[3] : NULL;
    if (third_arg && strcmp(third_arg, "frc") == 0) {
      extern int dg_frc;
      dg_frc = 1;
      datagen(tokens[1], (uint64_t)atof(tokens[2]), NULL);
    } else if (third_arg && strcmp(third_arg, "dfrc") == 0) {
      extern int dg_dfrc;
      dg_dfrc = 1;
      datagen(tokens[1], (uint64_t)atof(tokens[2]), NULL);
    } else {
      datagen(tokens[1], (uint64_t)atof(tokens[2]), third_arg);
    }
  }
  else {
    printf("info string unknown command: %s\n", cmd);
  }
  
  return true;

}
