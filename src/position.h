#ifndef POSITION_H
#define POSITION_H

#include "nodes.h"

int fen_board_valid(const char *board_fen);
void position(Node *node, const char *board_fen, const char *stm_str, const char *rights_str, const char *ep_str, int hmc, int num_uci_moves, char **uci_moves);

#endif
