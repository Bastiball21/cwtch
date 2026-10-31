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

#define INPUT_BUFFER_SIZE 8192

int main(int argc, char *argv[]) {

  setbuf(stdin, NULL);
  setbuf(stdout, NULL);

  init_attacks();
  init_weights();
  init_zob();
  init_lmr();
  init_line_masks();

  position(&nodes[0], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", 0, 0, NULL);

  // If command-line arguments provided, execute them as a single command and exit
  if (argc > 1) {
    char cmd[INPUT_BUFFER_SIZE];
    cmd[0] = '\0';
    for (int i = 1; i < argc; i++) {
      strcat(cmd, argv[i]);
      if (i < argc - 1) strcat(cmd, " ");
    }
    uci_exec(cmd);
    return 0;
  }

  // Otherwise run interactive loop
  char input[INPUT_BUFFER_SIZE];

  input_init();

  while (input_get(input, sizeof(input))) {
    if (!uci_exec(input)) {
      break;
    }
  }

  _Exit(0);
}