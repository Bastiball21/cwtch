#ifndef MAKEMOVE_H
#define MAKEMOVE_H

#include "nodes.h"
#include "move.h"

void make_move(Node *node, const move_t move);
void apply_move(Node *node, const move_t move);
void play_move(Node *node, char *uci_move);
void make_null_move(Position *pos);

#endif
