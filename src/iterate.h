#ifndef ITERATE_H
#define ITERATE_H

#include "move.h"
#include "nodes.h"

void init_next_search_move(Node *node, const int in_check, const move_t tt_move);
void init_next_qsearch_move(Node *node, const int in_check, const move_t tt_move);
move_t get_next_search_move(Node *node);
move_t get_next_qsearch_move(Node *node);
int is_bad_noisy_stage(const Node *node);
void restart_after_tt_move(Node *node, const move_t tt_move);

#endif
