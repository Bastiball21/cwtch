#include "bitboard.h"
#include "see.h"
#include "move.h"

// The king is worth more than anything it could win, so a capture by the king
// never looks good while the square is still defended.
const int see_values[12] = {100, 325, 325, 500, 1000, 20000, 100, 325, 325, 500, 1000, 20000};

static uint64_t rank_mask[64];
static uint64_t file_mask[64];
static uint64_t diag_mask[64];
static uint64_t anti_mask[64];

void init_line_masks(void) {

  for (int sq=0; sq < 64; sq++) {

    int r       = sq >> 3;
    int f       = sq & 7;
    uint64_t rm = 0;
    uint64_t fm = 0;
    uint64_t dm = 0;
    uint64_t am = 0;

    for (int k=0; k < 8; k++) {
      rm |= 1ULL << (r * 8 + k);     // same rank
      fm |= 1ULL << (k * 8 + f);     // same file
    }

    for (int rr=r+1, ff=f+1; rr<8  && ff<8;  rr++, ff++) dm |= 1ULL << (rr*8+ff);
    for (int rr=r-1, ff=f-1; rr>=0 && ff>=0; rr--, ff--) dm |= 1ULL << (rr*8+ff);
    for (int rr=r+1, ff=f-1; rr<8  && ff>=0; rr++, ff--) am |= 1ULL << (rr*8+ff);
    for (int rr=r-1, ff=f+1; rr>=0 && ff<8;  rr--, ff++) am |= 1ULL << (rr*8+ff);

    rank_mask[sq] = rm;
    file_mask[sq] = fm;
    diag_mask[sq] = dm;
    anti_mask[sq] = am;

  }

}

static uint64_t get_least_valuable_piece(const Position *const pos, const uint64_t attadef, const int by_side, int *piece) {

  const int base = by_side ? 6 : 0;

  if ((attadef & pos->colour[by_side]) == 0)
    return 0;

  for (int i=base; i < base + 6; i++) {
    uint64_t subset = attadef & pos->all[i];
    if (subset) {
      *piece = i;
      return subset & -subset;
    }
  }

  return 0;

}

static uint64_t rook_attackers_to(const Position *const pos, const uint64_t occ, const int to_sq) {

  const Attack *const restrict a = &rook_attacks[to_sq];
  uint64_t rays                  = a->attacks[magic_index(occ & a->mask, a->magic, a->shift)];

  return rays & (pos->all[ROOK] | pos->all[6+ROOK] | pos->all[QUEEN] | pos->all[6+QUEEN]);

}

static uint64_t bishop_attackers_to(const Position *const pos, const uint64_t occ, const int to_sq) {

  const Attack *const restrict a = &bishop_attacks[to_sq];
  uint64_t rays                  = a->attacks[magic_index(occ & a->mask, a->magic, a->shift)];

  return rays & (pos->all[BISHOP] | pos->all[6+BISHOP] | pos->all[QUEEN] | pos->all[6+QUEEN]);

}

static uint64_t static_attackers_to(const Position *const pos, const int to_sq) {

  uint64_t attackers = 0ULL;

  const uint64_t kn = knight_attacks[to_sq];

  attackers |= pos->all[KNIGHT]   & kn;
  attackers |= pos->all[6+KNIGHT] & kn;

  const uint64_t k = king_attacks[to_sq];

  attackers |= pos->all[KING]   & k;
  attackers |= pos->all[6+KING] & k;

  attackers |= pos->all[PAWN]   & pawn_attacks[WHITE][to_sq];
  attackers |= pos->all[6+PAWN] & pawn_attacks[BLACK][to_sq];

  return attackers;

}

int see_ge(const Position *const pos, const move_t move, int threshold) {

  // EP, promotions and castling: don't filter (return "passes threshold")
  if (move & (MOVE_FLAG_EPCAPTURE | MOVE_FLAG_PROMOTE | MOVE_FLAG_CASTLE))
    return 1;

  const int stm     = pos->stm;
  const int from_sq = (move >> 6) & 0x3F;
  const int to_sq   = move & 0x3F;
  int attacker      = pos->board[from_sq];
  const int target  = pos->board[to_sq];
  const int gained  = target == EMPTY ? 0 : see_values[target];

  if (gained - see_values[attacker] >= threshold)
    return 1;

  uint64_t used           = 0ULL;
  uint64_t occ            = pos->occupied;
  uint64_t from_set       = 1ULL << from_sq;
  const uint64_t stat_atk = static_attackers_to(pos, to_sq);
  uint64_t rook_atk       = rook_attackers_to(pos, occ, to_sq);
  uint64_t bish_atk       = bishop_attackers_to(pos, occ, to_sq);
  uint64_t attadef        = stat_atk | rook_atk | bish_atk | from_set;  // a quiet pawn push doesn't attack `to`

  int gain[32], d = 0;
  gain[0] = gained - threshold;

  do {

    d++;
    gain[d] = see_values[attacker] - gain[d-1];

    if ((gain[d] < 0) && (-gain[d-1] < 0))
      break;

    used    |= from_set;
    attadef ^= from_set;
    occ     ^= from_set;

    const uint64_t moved_bb = from_set;

    if (moved_bb & (rank_mask[to_sq] | file_mask[to_sq]))
      rook_atk = rook_attackers_to(pos, occ, to_sq);

    if (moved_bb & (diag_mask[to_sq] | anti_mask[to_sq]))
      bish_atk = bishop_attackers_to(pos, occ, to_sq);

    attadef  = (stat_atk | rook_atk | bish_atk) & ~used;
    from_set = get_least_valuable_piece(pos, attadef, (stm ^ (d & 1)), &attacker);

  } while (from_set);

  while (--d)
    gain[d-1] = -(( -gain[d-1] > gain[d]) ? -gain[d-1] : gain[d]);

  return gain[0] >= 0;

}
