
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "nodes.h"
#include "makemove.h"
#include "zobrist.h"
#include "net.h"
#include "hh.h"
#include "builtins.h"

int fen_board_valid(const char *board_fen) {

  int rank = 7;
  int file = 0;
  int kings[2] = {0, 0};

  for (const char *p = board_fen; *p; ++p) {

    if (*p == '/') {
      if (file != 8 || rank == 0)
        return 0;
      rank--;
      file = 0;
    }
    else if (*p >= '1' && *p <= '8') {
      file += *p - '0';
    }
    else if (strchr("pnbrqkPNBRQK", *p)) {
      if (*p == 'K') kings[WHITE]++;
      if (*p == 'k') kings[BLACK]++;
      file++;
    }
    else {
      return 0;
    }

    if (file > 8)
      return 0;

  }

  return rank == 0 && file == 8 && kings[WHITE] == 1 && kings[BLACK] == 1;

}

void position(Node *node, const char *board_fen, const char *stm_str, const char *rights_str, const char *ep_str, int hmc, int num_uci_moves, char **uci_moves) {

  Position *pos = &node->pos;

  static const int char_to_piece[128] = {
    ['p'] = 0, ['n'] = 1, ['b'] = 2, ['r'] = 3, ['q'] = 4, ['k'] = 5,
  };

  memset(pos, 0, sizeof(Position));

  // board
  
  for (int i=0; i < 64; i++)
    pos->board[i] = EMPTY;
  
  int sq = 56;
  
  for (const char *p = board_fen; *p; ++p) {
  
    if (*p == '/') {
      sq -= 16;
    }
  
    else if (isdigit(*p)) {
      sq += *p - '0';
    }
  
    else {
  
      int colour = !!islower(*p);
      int piece  = char_to_piece[tolower(*p)];  // 0-5
      int index  = piece_index(piece, colour);  // 0-11
  
      uint64_t bb = 1ULL << sq;
  
      pos->all[index] |= bb;
      pos->occupied |= bb;
      pos->colour[colour] |= bb;
  
      pos->board[sq] = index;
  
      sq++;
  
    }
  }
  
  // stm
  
  pos->stm = (stm_str[0] == 'w') ? WHITE : BLACK;
  
  // rights
  
  pos->rights = 0;

  extern int is_chess960;
  pos->is_chess960 = is_chess960;

  // Initialize rook squares to default A/H files
  pos->castling_rook_sq[WHITE][0] = H1; // Kingside
  pos->castling_rook_sq[WHITE][1] = A1; // Queenside
  pos->castling_rook_sq[BLACK][0] = H8;
  pos->castling_rook_sq[BLACK][1] = A8;

  int w_king_sq = bsf(pos->all[piece_index(KING, WHITE)]);
  int b_king_sq = bsf(pos->all[piece_index(KING, BLACK)]);
  int w_king_file = w_king_sq % 8;
  int b_king_file = b_king_sq % 8;

  // Mask rooks strictly to their starting back ranks (Rank 1 for White, Rank 8 for Black)
  uint64_t w_rank1_rooks = pos->all[piece_index(ROOK, WHITE)] & 0xFFULL;
  uint64_t b_rank8_rooks = pos->all[piece_index(ROOK, BLACK)] & 0xFF00000000000000ULL;

  for (const char *p = rights_str; *p; ++p) {
    if (*p == 'K') {
      if (w_rank1_rooks) {
        int r_sq = msb(w_rank1_rooks);
        if (r_sq > w_king_sq) {
          pos->rights |= WHITE_RIGHTS_KING;
          pos->castling_rook_sq[WHITE][0] = r_sq;
        }
      }
    }
    else if (*p == 'Q') {
      if (w_rank1_rooks) {
        int r_sq = bsf(w_rank1_rooks);
        if (r_sq < w_king_sq) {
          pos->rights |= WHITE_RIGHTS_QUEEN;
          pos->castling_rook_sq[WHITE][1] = r_sq;
        }
      }
    }
    else if (*p == 'k') {
      if (b_rank8_rooks) {
        int r_sq = msb(b_rank8_rooks);
        if (r_sq > b_king_sq) {
          pos->rights |= BLACK_RIGHTS_KING;
          pos->castling_rook_sq[BLACK][0] = r_sq;
        }
      }
    }
    else if (*p == 'q') {
      if (b_rank8_rooks) {
        int r_sq = bsf(b_rank8_rooks);
        if (r_sq < b_king_sq) {
          pos->rights |= BLACK_RIGHTS_QUEEN;
          pos->castling_rook_sq[BLACK][1] = r_sq;
        }
      }
    }
    // X-FEN / Shredder-FEN Chess960 Castling Rights (A-H)
    else if (*p >= 'A' && *p <= 'H') {
      int r_file = *p - 'A';
      int r_sq = A1 + r_file;
      pos->is_chess960 = 1;
      if (r_file > w_king_file) {
        pos->rights |= WHITE_RIGHTS_KING;
        pos->castling_rook_sq[WHITE][0] = r_sq;
      } else {
        pos->rights |= WHITE_RIGHTS_QUEEN;
        pos->castling_rook_sq[WHITE][1] = r_sq;
      }
    }
    else if (*p >= 'a' && *p <= 'h') {
      int r_file = *p - 'a';
      int r_sq = A8 + r_file;
      pos->is_chess960 = 1;
      if (r_file > b_king_file) {
        pos->rights |= BLACK_RIGHTS_KING;
        pos->castling_rook_sq[BLACK][0] = r_sq;
      } else {
        pos->rights |= BLACK_RIGHTS_QUEEN;
        pos->castling_rook_sq[BLACK][1] = r_sq;
      }
    }
  }
  
  // ep

  if (ep_str[0] >= 'a' && ep_str[0] <= 'h' && (ep_str[1] == '3' || ep_str[1] == '6')) {

    int file = ep_str[0] - 'a';
    int rank = ep_str[1] - '1';
    int ep_sq = rank * 8 + file;

    // only set EP if an enemy pawn can actually capture
    int cap_pawn = piece_index(PAWN, pos->stm);
    int pawn_rank_sq = ep_sq + (pos->stm == WHITE ? -8 : 8);
    uint64_t pawn_bb = 1ULL << pawn_rank_sq;
    uint64_t adj = ((pawn_bb & NOT_A_FILE) >> 1) | ((pawn_bb & NOT_H_FILE) << 1);
    if (pos->all[cap_pawn] & adj)
      pos->ep = ep_sq;

  }

  // hmc
  pos->hmc = hmc < 0 ? 0 : (hmc > 255 ? 255 : hmc);

  pos->hash = rebuild_hash(pos);
  net_slow_rebuild_accs(node);
  node->accs_dirty = 0;
  node->prev_piece = EMPTY; /* Root has no previous move */
  node->prev_to = 0;

  // initialize hh with starting position
  hh_reset();
  hh_push(pos->hash);

  // play uci moves
  for (int m = 0; m < num_uci_moves; m++) {
    if (!play_move(node, uci_moves[m])) {
      printf("info string illegal move %s, ignoring the rest of the line\n", uci_moves[m]);
      break;
    }
    hh_push(pos->hash);
  }

}
