#include <string.h>
#include "builtins.h"
#include "nodes.h"
#include "move.h"
#include "types.h"
#include "iterate.h"
#include "zobrist.h"
#include "net.h"

#define MOVE_FLAGS_PROMOCAP (MOVE_FLAG_PROMOTE | MOVE_FLAG_CAPTURE) 

void make_move(Node *node, const move_t move) {

  Position *pos = &node->pos;
  uint8_t *board = pos->board;
  uint64_t *all = pos->all;
  uint64_t *colour = pos->colour;
  const int stm = pos->stm;
  const int opp = stm ^ 1;
  const int flags = move & 0xFFF000;
  const int from = (move >> 6) & 0x3F;
  const int to = move & 0x3F;
  const int from_piece = board[from];
  const int to_piece = board[to];
  const uint64_t from_bb = 1ULL << from;
  const uint64_t to_bb = 1ULL << to;
  const uint64_t move_bb = from_bb ^ to_bb;
  const uint8_t old_rights = pos->rights;
  uint64_t hash = pos->hash ^ zob_pieces[from_piece][from] ^ zob_ep[pos->ep] ^ zob_rights[old_rights];

  pos->ep = 0;

  uint8_t *nd_args = node->net_deferred.args;

  if (flags & MOVE_FLAGS_EXTRA) {

    if ((flags & MOVE_FLAGS_EXTRA) == MOVE_FLAG_CAPTURE) {  // most common
      board[from] = EMPTY;
      board[to] = from_piece;
      all[from_piece] ^= move_bb;
      all[to_piece] ^= to_bb;
      colour[stm] ^= move_bb;
      colour[opp] ^= to_bb;
      hash ^= zob_pieces[from_piece][to] ^ zob_pieces[to_piece][to];
      node->net_deferred.type = NET_OP_CAPTURE;
      nd_args[0] = from_piece;
      nd_args[1] = from;
      nd_args[2] = to_piece;
      nd_args[3] = to;
    }

    else if (flags & MOVE_FLAG_EPCAPTURE) {
      const int cap_sq = to + (stm ? 8 : -8);
      const uint64_t cap_bb = 1ULL << cap_sq;
      const int cap_piece = board[cap_sq];
      board[from] = EMPTY;
      board[to] = from_piece;
      board[cap_sq] = EMPTY;
      all[from_piece] ^= move_bb;
      all[cap_piece] ^= cap_bb;
      colour[stm] ^= move_bb;
      colour[opp] ^= cap_bb;
      hash ^= zob_pieces[from_piece][to] ^ zob_pieces[cap_piece][cap_sq];
      node->net_deferred.type = NET_OP_EP_CAPTURE;
      nd_args[0] = from_piece;
      nd_args[1] = from;
      nd_args[2] = to;
      nd_args[3] = cap_piece;
      nd_args[4] = cap_sq;
    }

    else if ((flags & MOVE_FLAGS_PROMOCAP) == MOVE_FLAGS_PROMOCAP) {
      const int promo_piece = piece_index((flags >> 12) & 7, stm);
      board[from] = EMPTY;
      board[to] = promo_piece;
      all[from_piece] ^= from_bb;
      all[to_piece] ^= to_bb;
      all[promo_piece] ^= to_bb;
      colour[stm] ^= move_bb;
      colour[opp] ^= to_bb;
      hash ^= zob_pieces[to_piece][to] ^ zob_pieces[promo_piece][to];
      node->net_deferred.type = NET_OP_PROMO_CAPTURE;
      nd_args[0] = from_piece;
      nd_args[1] = from;
      nd_args[2] = to;
      nd_args[3] = to_piece;
      nd_args[4] = promo_piece;
    }

    else if (flags & MOVE_FLAG_PROMOTE) {
      const int promo_piece = piece_index((flags >> 12) & 7, stm);
      board[from] = EMPTY;
      board[to] = promo_piece;
      all[from_piece] ^= from_bb;
      all[promo_piece] ^= to_bb;
      colour[stm] ^= move_bb;
      hash ^= zob_pieces[promo_piece][to];
      node->net_deferred.type = NET_OP_PROMO_PUSH;
      nd_args[0] = from_piece;
      nd_args[1] = from;
      nd_args[2] = to;
      nd_args[3] = promo_piece;
    }

    else if (flags & MOVE_FLAG_CASTLE) {
      // In Chess960 encoding, 'from' is King, 'to' is Rook
      const int k_to = (to > from) ? G1 + (stm * 56) : C1 + (stm * 56);
      const int r_to = (to > from) ? F1 + (stm * 56) : D1 + (stm * 56);
      
      const int king_piece = board[from];
      const int rook_piece = board[to];

      // 1. Remove pieces from their start squares FIRST (avoids overlap bugs)
      board[from] = EMPTY;
      board[to] = EMPTY;
      all[king_piece] ^= (1ULL << from);
      all[rook_piece] ^= (1ULL << to);
      colour[stm] ^= ((1ULL << from) | (1ULL << to));
      
      // 2. Place pieces on their final squares
      board[k_to] = king_piece;
      board[r_to] = rook_piece;
      all[king_piece] ^= (1ULL << k_to);
      all[rook_piece] ^= (1ULL << r_to);
      colour[stm] ^= ((1ULL << k_to) | (1ULL << r_to));

      hash ^= zob_pieces[king_piece][k_to] 
            ^ zob_pieces[rook_piece][to] ^ zob_pieces[rook_piece][r_to];

      node->net_deferred.type = NET_OP_CASTLE;
      nd_args[0] = king_piece;
      nd_args[1] = from;
      nd_args[2] = k_to;
      nd_args[3] = rook_piece;
      nd_args[4] = to;
      nd_args[5] = r_to;
    }

    else {
      // pawn2
      board[from] = EMPTY;
      board[to] = from_piece;
      all[from_piece] ^= move_bb;
      colour[stm] ^= move_bb;
      hash ^= zob_pieces[from_piece][to];
      const int opp_pawn = piece_index(PAWN, opp);
      const uint64_t adj = ((to_bb & NOT_A_FILE) >> 1) | ((to_bb & NOT_H_FILE) << 1);
      if (all[opp_pawn] & adj)
        pos->ep = (from + to) >> 1;
      node->net_deferred.type = NET_OP_MOVE;
      nd_args[0] = from_piece;
      nd_args[1] = from;
      nd_args[2] = to;
    }

  }

  else {
    // quiet move
    board[from] = EMPTY;
    board[to] = from_piece;
    all[from_piece] ^= move_bb;
    colour[stm] ^= move_bb;
    hash ^= zob_pieces[from_piece][to];
    node->net_deferred.type = NET_OP_MOVE;
    nd_args[0] = from_piece;
    nd_args[1] = from;
    nd_args[2] = to;
  }

  if (from_piece == WKING) pos->rights &= ~(WHITE_RIGHTS_KING | WHITE_RIGHTS_QUEEN);
  if (from_piece == BKING) pos->rights &= ~(BLACK_RIGHTS_KING | BLACK_RIGHTS_QUEEN);
  
  if (from == pos->castling_rook_sq[WHITE][0] || to == pos->castling_rook_sq[WHITE][0]) pos->rights &= ~WHITE_RIGHTS_KING;
  if (from == pos->castling_rook_sq[WHITE][1] || to == pos->castling_rook_sq[WHITE][1]) pos->rights &= ~WHITE_RIGHTS_QUEEN;
  if (from == pos->castling_rook_sq[BLACK][0] || to == pos->castling_rook_sq[BLACK][0]) pos->rights &= ~BLACK_RIGHTS_KING;
  if (from == pos->castling_rook_sq[BLACK][1] || to == pos->castling_rook_sq[BLACK][1]) pos->rights &= ~BLACK_RIGHTS_QUEEN;
  // ==========================================
  pos->occupied = colour[WHITE] | colour[BLACK];
  pos->stm = opp;
  pos->hash = hash ^ zob_rights[pos->rights] ^ zob_ep[pos->ep] ^ zob_stm[1];

  // update half-move clock
  const int is_pawn = (from_piece == WPAWN) || (from_piece == BPAWN);
  const int is_capture = flags & MOVE_FLAG_CAPTURE;
  pos->hmc = (is_pawn || is_capture) ? 0 : pos->hmc + 1;

}

void apply_move(Node *node, const move_t move) {
  make_move(node, move);
  int16_t scratch[2][NET_H1_SIZE];
  memcpy(scratch, node->accs, sizeof scratch);
  update_accs(node, scratch);
}

int play_move(Node *node, const char *uci_move) {

  char buf[6];

  Position *pos = &node->pos;
  const int stm = pos->stm;
  const int opp = stm ^ 1;
  const int stm_king_idx = piece_index(KING, stm);
  const int in_check = is_attacked(pos, bsf(pos->all[stm_king_idx]), opp);
  move_t move;
  Node scratch;

  init_next_search_move(node, in_check, 0);

  while ((move = get_next_search_move(node))) {

    format_move(move, buf);

    if (strcmp(uci_move, buf) != 0)
      continue;

    pos_copy(pos, &scratch.pos);
    make_move(&scratch, move);

    if (is_attacked(&scratch.pos, bsf(scratch.pos.all[stm_king_idx]), opp))
      return 0;

    apply_move(node, move);
    return 1;

  }

  return 0;

}

move_t first_legal_move(Node *node) {

  const Position *pos = &node->pos;
  const int stm = pos->stm;
  const int opp = stm ^ 1;
  const int king_idx = piece_index(KING, stm);
  Node scratch;
  move_t move;

  init_next_search_move(node, is_attacked(pos, bsf(pos->all[king_idx]), opp), 0);

  while ((move = get_next_search_move(node))) {
    pos_copy(pos, &scratch.pos);
    make_move(&scratch, move);
    if (!is_attacked(&scratch.pos, bsf(scratch.pos.all[king_idx]), opp))
      return move;
  }

  return 0;

}

void make_null_move(Position *pos) {

  int ep = pos->ep;
  uint64_t hash = pos->hash;

  hash ^= zob_ep[ep];
  ep = 0;
  hash ^= zob_ep[ep];

  hash ^= zob_stm[1];

  pos->hash = hash;
  pos->stm ^= 1;
  pos->ep = (uint8_t)ep;
  pos->hmc = 0;

}
