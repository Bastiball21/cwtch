#ifndef NODES_H
#define NODES_H

#include "types.h"
#include "pos.h"
#include "move.h"

#define MAX_PLY 128
#define MAX_MOVES 256

enum {
  NET_OP_MOVE,
  NET_OP_CAPTURE,
  NET_OP_EP_CAPTURE,
  NET_OP_CASTLE,
  NET_OP_PROMO_PUSH,
  NET_OP_PROMO_CAPTURE,
};

typedef struct {
  uint8_t type;
  uint8_t args[6];
} NetDeferred;

typedef struct {

  _Alignas(64) int16_t accs[2][NET_H1_SIZE];  // avoid cache line splits
  Position pos;
  NetDeferred net_deferred;
  uint8_t accs_dirty;
  move_t moves[MAX_MOVES];
  move_t played[MAX_MOVES];
  move_t bad_noisy[MAX_MOVES];  // losing captures and underpromotions, tried last
  int num_moves;
  int32_t ranks[MAX_MOVES];
  int next_move;
  int num_bad;
  int next_bad;
  int skip_quiets;
  int in_check;
  move_t tt_move;
  move_t excluded_move;  // se verification
  int dextensions;       // double extensions on path
  int stage;
  int16_t ev;  // static eval
  move_t killer;
  int16_t (*cont_entry)[64];  // history after the previous move; NULL at root and after null move
  int16_t (*cont2)[64];       // ... after the move 2 plies back
  int16_t (*cont4)[64];       // ... after the move 4 plies back

  // move that led to this node, for the countermove table
  int prev_piece;
  int prev_to;

} Node;

extern _Thread_local Node nodes[MAX_PLY];

void clear_nodes(void);

#endif
