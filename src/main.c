#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "uci.h"
#include "bitboard.h"
#include "nodes.h"
#include "net.h"
#include "zobrist.h"
#include "search.h"
#include "see.h"
#include "input.h"
#include "position.h"
#include "datagen.h"

int main(int argc, char *argv[]) {

  setbuf(stdin, NULL);
  setbuf(stdout, NULL);

  init_attacks();
  if (init_weights() != 0)
    return 1;
  init_zob();
  init_lmr();
  init_line_masks();

  position(&nodes[0], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", 0, 0, NULL);

  // If command-line arguments provided, execute them as a single command and exit
  if (argc > 1) {
    static char cmd[INPUT_BUF_SIZE];
    size_t used = 0;
    for (int i = 1; i < argc && used < sizeof(cmd); i++)
      used += (size_t)snprintf(cmd + used, sizeof(cmd) - used, "%s%s", i > 1 ? " " : "", argv[i]);
    uci_exec(cmd);
    return 0;
  }

  // Otherwise run interactive loop
  static char input[INPUT_BUF_SIZE];

  input_init();

  while (input_get(input, sizeof(input))) {
    if (!uci_exec(input)) {
      break;
    }
  }

  _Exit(0);
}