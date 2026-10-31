#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <strings.h>
#include <zstd.h>
#include <unistd.h>   // for getpid()
#include <signal.h>   // for SIGINT/SIGTERM handling
#include <time.h>     // for manifest timestamp

#include "datagen.h"
#include "types.h"
#include "builtins.h"
#include "nodes.h"
#include "pos.h"
#include "move.h"
#include "makemove.h"
#include "tt.h"
#include "hh.h"
#include "timecontrol.h"
#include "go.h"
#include "position.h"
#include "iterate.h"
#include "net.h"
#include "uci.h"

// ---------------------------------------------------------------------------
// Threading and signal model
// ---------------------------------------------------------------------------
// All state in this file is process-local. Parallelism is achieved by launching
// N independent `cwtch` processes (see run_datagen_*.sh), NOT by pthreads.
// If this ever becomes multithreaded, every static below needs per-thread
// copies: bloom_filter, dg_seed, book_fens, book_order, dg_frc, dg_dfrc.
//
// SIGINT/SIGTERM sets dg_stop_requested. The main loop checks it once per game,
// so shutdown latency equals however long the in-flight game takes to finish
// . The check is deliberately NOT inside the ply loop — that would add a branch 
// to the hot path for no data-integrity benefit. Once the current game completes, 
// the main loop breaks and runs the normal flush / ZSTD_endStream / fclose path, 
// so the output file is a clean,complete zstd frame.
// ---------------------------------------------------------------------------

extern int is_chess960;

// Nodes per move. Soft: the current iteration finishes, so the score comes from a
// complete search instead of one cut off part way through. The hard limit only
// guards against a search that explodes.
#define DG_SEARCH_NODES        5000
#define DG_HARD_NODES          100000
#define DG_DRAW_SCORE          10
#define DG_DRAW_COUNT          10
#define DG_DRAW_PLY            30
#define DG_MAX_GAME_MOVES      512
#define DG_REPORT_SECS         10
#define DG_FILE_PREFIX         "data"
#define DG_DOWNSAMPLE_RATE     1

// Adjudication thresholds.
//   DG_ADJUDICATE_SCORE — |score| above this counts as "winning" toward adjudication.
//   DG_ADJUDICATE_COUNT — number of consecutive plies above that score to end the game.
#define DG_ADJUDICATE_SCORE    1500
#define DG_ADJUDICATE_COUNT    3

// Random-opening plies used in the no-book fallback path.
// Pick a length uniformly from [MIN, MAX] inclusive.
#define DG_RANDOM_PLIES_MIN    10
#define DG_RANDOM_PLIES_MAX    25

// Minimum ply from which positions become training samples.
//
// The value is measured from the start of the recording loop, i.e. AFTER
// the opening book position and the 0–3 extra random plies. For UHO, a book
// position sits around move 10–15, so `min_record_ply 10` means recording
// starts around real move 16–18 — past the book, past the immediate mainline
// continuation, and into the game's own development.
//
//   Standard (book-based): 10
//   FRC / DFRC:             8  — Chess960 has no opening theory, so early
//                                 plies carry real signal.
#define DG_MIN_RECORD_PLY_STD  10
#define DG_MIN_RECORD_PLY_FRC  8

// Per-mode extra random plies after the starting position.
//   DFRC: 4  — 921,600 starts already provide enormous variety
//   FRC:  8  — only 960 starts; more extra plies add needed diversity
//   Book: 3  — keep close to real opening play
#define DG_EXTRA_PLIES_DFRC    4
#define DG_EXTRA_PLIES_FRC     8
#define DG_EXTRA_PLIES_BOOK    3

// Sentinel score for plies that must be replayed but not trained on.
// Must never collide with a real search score. Real scores are clamped to
// ±MATE (32000), so INT16_MIN (-32768) has a ~768-point safety margin.
// The static assert below enforces the invariant at compile time.
#define DG_SENTINEL_SCORE      INT16_MIN

_Static_assert(
    DG_SENTINEL_SCORE < -MATE || DG_SENTINEL_SCORE > MATE,
    "DG_SENTINEL_SCORE must never collide with a real search score"
);

// Bloom filter for uniqueness
#define BLOOM_SIZE_BYTES       (1ULL << 26)   // 64 MB
#define BLOOM_RESET_INSERTS    50000000ULL

static uint8_t *bloom_filter = NULL;

// Epoch counters — reset at every bloom reset, used for the live dup-rate
// display so the ratio stays meaningful within the current filter generation.
static uint64_t bloom_insertions = 0;      // accepted (unique) in this epoch
static uint64_t dg_duplicate_rejects = 0;  // rejected in this epoch

// Lifetime counters — never reset, used for the end-of-run summary and the
// manifest result block.
static uint64_t dg_total_queries = 0;
static uint64_t dg_total_duplicates = 0;
static uint64_t dg_fallback_moves = 0;

// --- Signal handling ---
static volatile sig_atomic_t dg_stop_requested = 0;

// Set when the disk rejects a write: games after that point would be lost silently.
static int dg_write_failed = 0;

static void dg_fwrite(const void *data, size_t size, FILE *fp) {
    if (size && fwrite(data, 1, size, fp) != size && !dg_write_failed) {
        fprintf(stderr, "error: write failed, is the disk full? stopping datagen\n");
        dg_write_failed = 1;
    }
}

static void dg_handle_signal(int sig) {
    (void)sig;
    dg_stop_requested = 1;
}

#define VIRI_TYPE_NORMAL       0
#define VIRI_TYPE_EP           1
#define VIRI_TYPE_CASTLE       2
#define VIRI_TYPE_PROMO        3

#define VIRI_WDL_BLACK_WIN     0
#define VIRI_WDL_DRAW          1
#define VIRI_WDL_WHITE_WIN     2

// --- RNG ---
static uint64_t dg_seed;

static void dg_seed_rng(void) {
    uint64_t urand = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&urand, sizeof(urand), 1, f)) {}
        fclose(f);
    }
    uint64_t x;
    dg_seed = time_ms() ^ (uint64_t)(uintptr_t)&x ^ ((uint64_t)getpid() << 32) ^ urand;
    if (!dg_seed) dg_seed = 1;
}

static uint64_t dg_rand(void) {
    dg_seed ^= dg_seed >> 12;
    dg_seed ^= dg_seed << 25;
    dg_seed ^= dg_seed >> 27;
    return dg_seed * 2685821657736338717ULL;
}

// --- Bloom filter functions ---
static void init_bloom(void) {
    bloom_filter = calloc(BLOOM_SIZE_BYTES, 1);
    if (!bloom_filter) {
        fprintf(stderr, "error: failed to allocate bloom filter\n");
        exit(1);
    }
}

static void free_bloom(void) {
    free(bloom_filter);
    bloom_filter = NULL;
}

// 2-hash Bloom filter. Uses two bit positions derived from the same 64-bit hash.
// At the 50M-insertion reset point about 17% of the bits are set, so about 3% of
// genuinely new positions are wrongly rejected just before a reset (under 1% on
// average over the filter's life).
//
// Epoch counters (bloom_insertions, dg_duplicate_rejects) are reset together
// with the filter so the live dup-rate display stays meaningful. Lifetime
// counters (dg_total_queries, dg_total_duplicates) accumulate across the
// whole run for the final summary and manifest.
static int is_unique_and_add(uint64_t hash) {
    dg_total_queries++;

    // Second hash via the golden-ratio Knuth multiplier — good bit mixing.
    const uint64_t h2 = hash * 0x9E3779B97F4A7C15ULL;

    // Bit indices into the filter (BLOOM_SIZE_BYTES * 8 total bits).
    const uint64_t bit1 = hash & (BLOOM_SIZE_BYTES * 8 - 1);
    const uint64_t bit2 = h2   & (BLOOM_SIZE_BYTES * 8 - 1);

    const uint64_t byte1 = bit1 >> 3;
    const uint64_t byte2 = bit2 >> 3;
    const int mask1 = 1 << (bit1 & 7);
    const int mask2 = 1 << (bit2 & 7);

    // Duplicate only if both bits are already set.
    if ((bloom_filter[byte1] & mask1) && (bloom_filter[byte2] & mask2)) {
        dg_duplicate_rejects++;
        dg_total_duplicates++;
        return 0;
    }

    bloom_filter[byte1] |= mask1;
    bloom_filter[byte2] |= mask2;
    bloom_insertions++;
    return 1;
}

// --- Book handling ---
#define MAX_BOOK_FENS 3000000

static char **book_fens = NULL;
static int book_count = 0;
static int book_capacity = 0;
static int *book_order = NULL;
static int book_order_len = 0;
static int book_order_pos = 0;

int dg_frc = 0;
int dg_dfrc = 0;

static int book_line_usable(const char *fen);

static void add_book_fen(const char *fen) {
    if (!book_line_usable(fen)) {
        fprintf(stderr, "warning: skipping invalid book line: %.80s\n", fen);
        return;
    }
    if (book_count >= MAX_BOOK_FENS) {
        fprintf(stderr, "warning: book exceeds %d positions, ignoring extras\n", MAX_BOOK_FENS);
        return;
    }

    if (book_count >= book_capacity) {
        int new_cap = book_capacity ? book_capacity * 2 : 1024;
        if (new_cap > MAX_BOOK_FENS) new_cap = MAX_BOOK_FENS;
        char **new_arr = realloc(book_fens, new_cap * sizeof(char *));
        if (!new_arr) {
            fprintf(stderr, "error: out of memory loading book\n");
            return;
        }
        book_fens = new_arr;
        book_capacity = new_cap;
    }

    book_fens[book_count] = strdup(fen);
    if (book_fens[book_count]) {
        book_count++;
    } else {
        fprintf(stderr, "error: strdup failed\n");
    }
}

static void load_book_epd(const char *book_file) {
    FILE *fp = fopen(book_file, "r");
    if (!fp) {
        printf("warning: cannot open book file %s, using random openings\n", book_file);
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r')) p[--len] = '\0';
        if (len == 0) continue;

        // Keep the first 6 FEN fields (board, stm, rights, ep, halfmove, fullmove).
        // Additional columns (EPD operations like "bm", "am", "id") are dropped.
        char fen_only[512];
        int token_count = 0;
        char *saveptr;
        char *token = strtok_r(p, " ", &saveptr);
        fen_only[0] = '\0';
        while (token && token_count < 6) {
            if (token_count > 0) strncat(fen_only, " ", sizeof(fen_only) - strlen(fen_only) - 1);
            strncat(fen_only, token, sizeof(fen_only) - strlen(fen_only) - 1);
            token = strtok_r(NULL, " ", &saveptr);
            token_count++;
        }

        add_book_fen(fen_only);
    }

    fclose(fp);
    printf("Loaded %d positions from book file %s\n", book_count, book_file);
}

static void load_book_pgn(const char *book_file) {
    FILE *fp = fopen(book_file, "r");
    if (!fp) {
        printf("warning: cannot open PGN book %s\n", book_file);
        return;
    }

    char line[1024];
    int added = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *fen_start = strstr(line, "[FEN");
        if (!fen_start) continue;
        char *quote1 = strchr(fen_start, '"');
        if (!quote1) continue;
        char *quote2 = strchr(quote1 + 1, '"');
        if (!quote2) continue;
        *quote2 = '\0';
        const char *fen = quote1 + 1;
        if (strlen(fen) > 0) {
            add_book_fen(fen);
            added++;
        }
    }

    fclose(fp);
    printf("Loaded %d positions from PGN book %s\n", added, book_file);
}

static void load_book(const char *book_file) {
    if (!book_file) return;
    const char *dot = strrchr(book_file, '.');
    if (dot && strcasecmp(dot, ".pgn") == 0) {
        load_book_pgn(book_file);
        return;
    }
    load_book_epd(book_file);
}

static void free_book(void) {
    for (int i = 0; i < book_count; i++) free(book_fens[i]);
    free(book_fens);
    book_fens = NULL;
    book_count = 0;
    book_capacity = 0;
    free(book_order);
    book_order = NULL;
    book_order_len = 0;
    book_order_pos = 0;
}

static void set_position_from_fen(Node *node, const char *fen) {
    char board[128], side[2], castling[8], ep[8];
    int hmc = 0;
    if (sscanf(fen, "%127s %1s %7s %7s", board, side, castling, ep) != 4) {
        printf("warning: invalid FEN in book: %s\n", fen);
        return;
    }
    const char *p = fen;
    for (int i = 0; i < 4; i++) {
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    if (*p) hmc = atoi(p);
    position(node, board, side, castling, ep, hmc, 0, NULL);
}

static int book_line_usable(const char *fen) {
    char board[128];
    return sscanf(fen, "%127s", board) == 1 && fen_board_valid(board);
}

static void shuffle_book_order(void) {
    for (int i = book_count - 1; i > 0; i--) {
        int j = dg_rand() % (i + 1);
        int tmp = book_order[i];
        book_order[i] = book_order[j];
        book_order[j] = tmp;
    }
    book_order_pos = 0;
}

static void set_random_book_position(void) {
    if (book_count == 0) return;
    if (book_order_len != book_count) {
        free(book_order);
        book_order = malloc(book_count * sizeof(int));
        if (!book_order) {
            fprintf(stderr, "error: out of memory for book order\n");
            return;
        }
        for (int i = 0; i < book_count; i++) book_order[i] = i;
        shuffle_book_order();
        book_order_len = book_count;
    }
    if (book_order_pos >= book_count) shuffle_book_order();
    int idx = book_order[book_order_pos++];
    set_position_from_fen(&nodes[0], book_fens[idx]);
}

// --- FRC / DFRC generators ---
static void generate_frc_fen(char *fen, size_t fen_size) {
    char pieces[8];
    memset(pieces, 0, sizeof(pieces));

    int dark = (dg_rand() % 4) * 2;
    int light = (dg_rand() % 4) * 2 + 1;
    pieces[dark] = 'b';
    pieces[light] = 'b';

    int rem[6], rem_cnt = 0;
    for (int i = 0; i < 8; i++) if (pieces[i] != 'b') rem[rem_cnt++] = i;
    pieces[rem[dg_rand() % rem_cnt]] = 'q';

    int rem2[5], rem2_cnt = 0;
    for (int i = 0; i < 8; i++) if (pieces[i] != 'b' && pieces[i] != 'q') rem2[rem2_cnt++] = i;
    for (int i = rem2_cnt - 1; i > 0; i--) {
        int j = dg_rand() % (i + 1);
        int tmp = rem2[i]; rem2[i] = rem2[j]; rem2[j] = tmp;
    }
    pieces[rem2[0]] = 'n';
    pieces[rem2[1]] = 'n';

    int rem3[3], rem3_cnt = 0;
    for (int i = 0; i < 8; i++) if (pieces[i] == 0) rem3[rem3_cnt++] = i;
    pieces[rem3[0]] = 'r';
    pieces[rem3[1]] = 'k';
    pieces[rem3[2]] = 'r';

    char rank1[9], rank8[9];
    for (int i = 0; i < 8; i++) rank1[i] = toupper(pieces[i]);
    rank1[8] = '\0';
    for (int i = 0; i < 8; i++) rank8[i] = tolower(pieces[i]);
    rank8[8] = '\0';

    int wk = -1;
    for (int i = 0; i < 8; i++) if (rank1[i] == 'K') wk = i;
    int wl = -1, wr = -1;
    for (int i = 0; i < 8; i++) if (rank1[i] == 'R') {
        if (i < wk) wl = i; else wr = i;
    }
    char wrts[3] = "";
    if (wl != -1) wrts[0] = 'A' + wl;
    if (wr != -1) wrts[1] = 'A' + wr;
    wrts[2] = '\0';

    int bk = -1;
    for (int i = 0; i < 8; i++) if (rank8[i] == 'k') bk = i;
    int bl = -1, br = -1;
    for (int i = 0; i < 8; i++) if (rank8[i] == 'r') {
        if (i < bk) bl = i; else br = i;
    }
    char brts[3] = "";
    if (bl != -1) brts[0] = 'a' + bl;
    if (br != -1) brts[1] = 'a' + br;
    brts[2] = '\0';

    snprintf(fen, fen_size, "%s/pppppppp/8/8/8/8/PPPPPPPP/%s w %s%s - 0 1",
             rank8, rank1, wrts, brts);
}

static void generate_dfrc_fen(char *fen, size_t fen_size) {
    char white[8], black[8];
    memset(white, 0, 8);
    memset(black, 0, 8);

    int dark = (dg_rand() % 4) * 2;
    int light = (dg_rand() % 4) * 2 + 1;
    white[dark] = 'B'; white[light] = 'B';

    int rem[6], rem_cnt = 0;
    for (int i = 0; i < 8; i++) if (white[i] != 'B') rem[rem_cnt++] = i;
    white[rem[dg_rand() % rem_cnt]] = 'Q';

    int rem2[5], rem2_cnt = 0;
    for (int i = 0; i < 8; i++) if (white[i] != 'B' && white[i] != 'Q') rem2[rem2_cnt++] = i;
    for (int i = rem2_cnt - 1; i > 0; i--) {
        int j = dg_rand() % (i + 1);
        int tmp = rem2[i]; rem2[i] = rem2[j]; rem2[j] = tmp;
    }
    white[rem2[0]] = 'N'; white[rem2[1]] = 'N';

    int rem3[3], rem3_cnt = 0;
    for (int i = 0; i < 8; i++) if (white[i] == 0) rem3[rem3_cnt++] = i;
    white[rem3[0]] = 'R'; white[rem3[1]] = 'K'; white[rem3[2]] = 'R';

    dark = (dg_rand() % 4) * 2;
    light = (dg_rand() % 4) * 2 + 1;
    black[dark] = 'b'; black[light] = 'b';

    rem_cnt = 0;
    for (int i = 0; i < 8; i++) if (black[i] != 'b') rem[rem_cnt++] = i;
    black[rem[dg_rand() % rem_cnt]] = 'q';

    rem2_cnt = 0;
    for (int i = 0; i < 8; i++) if (black[i] != 'b' && black[i] != 'q') rem2[rem2_cnt++] = i;
    for (int i = rem2_cnt - 1; i > 0; i--) {
        int j = dg_rand() % (i + 1);
        int tmp = rem2[i]; rem2[i] = rem2[j]; rem2[j] = tmp;
    }
    black[rem2[0]] = 'n'; black[rem2[1]] = 'n';

    rem3_cnt = 0;
    for (int i = 0; i < 8; i++) if (black[i] == 0) rem3[rem3_cnt++] = i;
    black[rem3[0]] = 'r'; black[rem3[1]] = 'k'; black[rem3[2]] = 'r';

    char rank1[9], rank8[9];
    for (int i = 0; i < 8; i++) rank1[i] = white[i];
    rank1[8] = '\0';
    for (int i = 0; i < 8; i++) rank8[i] = black[i];
    rank8[8] = '\0';

    int wk = -1;
    for (int i = 0; i < 8; i++) if (white[i] == 'K') wk = i;
    int wl = -1, wr = -1;
    for (int i = 0; i < 8; i++) if (white[i] == 'R') {
        if (i < wk) wl = i; else wr = i;
    }
    char wrts[3] = "";
    if (wl != -1) wrts[0] = 'A' + wl;
    if (wr != -1) wrts[1] = 'A' + wr;
    wrts[2] = '\0';

    int bk = -1;
    for (int i = 0; i < 8; i++) if (black[i] == 'k') bk = i;
    int bl = -1, br = -1;
    for (int i = 0; i < 8; i++) if (black[i] == 'r') {
        if (i < bk) bl = i; else br = i;
    }
    char brts[3] = "";
    if (bl != -1) brts[0] = 'a' + bl;
    if (br != -1) brts[1] = 'a' + br;
    brts[2] = '\0';

    snprintf(fen, fen_size, "%s/pppppppp/8/8/8/8/PPPPPPPP/%s w %s%s - 0 1",
             rank8, rank1, wrts, brts);
}

// --- Board packing (used for game header) ---
static void pos_to_packed_board(const Position *pos, uint8_t *buf, uint8_t wdl) {
    memset(buf, 0, 32);
    uint64_t occ = pos->occupied;
    memcpy(buf, &occ, 8);
    uint64_t tmp = occ;
    int idx = 0;
    while (tmp) {
        int sq = bsf(tmp);
        tmp &= tmp - 1;
        uint8_t piece = pos->board[sq];
        int color = piece_colour(piece);
        int type = piece_type(piece);
        if (type == ROOK) {
            if (sq == pos->castling_rook_sq[WHITE][0] && (pos->rights & WHITE_RIGHTS_KING))  type = 6;
            if (sq == pos->castling_rook_sq[WHITE][1] && (pos->rights & WHITE_RIGHTS_QUEEN)) type = 6;
            if (sq == pos->castling_rook_sq[BLACK][0] && (pos->rights & BLACK_RIGHTS_KING))  type = 6;
            if (sq == pos->castling_rook_sq[BLACK][1] && (pos->rights & BLACK_RIGHTS_QUEEN)) type = 6;
        }
        uint8_t nibble = (color << 3) | type;
        int byte_idx = 8 + (idx / 2);
        if (idx & 1) buf[byte_idx] |= nibble << 4;
        else buf[byte_idx] = nibble;
        idx++;
    }
    uint8_t ep = pos->ep ? pos->ep : 64;
    buf[24] = (pos->stm << 7) | (ep & 0x7F);
    buf[25] = pos->hmc;
    buf[30] = wdl;
}

// --- Move conversion ---
// Engine move_t layout:  bits 0-5 = to, bits 6-11 = from, bits 12-14 = promo piece,
//                        bit 15 = promote flag, bits 16+ = capture/ep/pawn2/castle flags.
// Viri  layout:          bits 0-5 = from, bits 6-11 = to, bits 12-13 = promo index,
//                        bits 14-15 = type (0=normal, 1=ep, 2=castle, 3=promo).
//
// For castle moves the engine already encodes `to` as the ROOK's from-square
// (see movegen.c: encode_move(k_from, r_from, MOVE_FLAG_CASTLE)), which is the
// FRC-safe convention. We pass `to` through unchanged; the trainer reads it as
// the rook's from-square and derives the king/rook destinations from (from, to).
static uint16_t move_to_viri(move_t move) {
    int from = (move >> 6) & 0x3F;
    int to = move & 0x3F;
    int type = VIRI_TYPE_NORMAL;
    int promo = 0;

    if (move & MOVE_FLAG_EPCAPTURE) {
        type = VIRI_TYPE_EP;
    }
    else if (move & MOVE_FLAG_CASTLE) {
        type = VIRI_TYPE_CASTLE;
        // No rewrite: engine already provides rook_from in `to`.
    }
    else if (move & MOVE_FLAG_PROMOTE) {
        type = VIRI_TYPE_PROMO;
        int piece = (move >> 12) & 0x7;
        promo = piece - 1;
    }

    return (uint16_t)(from | (to << 6) | (promo << 12) | (type << 14));
}

// --- Game record structures ---
typedef struct {
    uint16_t move;
    int16_t score;
} GameRecord;

// --- Move generation ---
static int gen_legal_moves(move_t *legal) {
    Node *node = &nodes[0];
    Position *pos = &node->pos;
    const int stm = pos->stm;
    const int opp = stm ^ 1;
    const int king_idx = piece_index(KING, stm);
    const int in_check = is_attacked(pos, bsf(pos->all[king_idx]), opp);
    init_next_search_move(node, in_check, 0);
    int count = 0;
    move_t move;
    Node tmp;
    while ((move = get_next_search_move(node))) {
        pos_copy(pos, &tmp.pos);
        make_move(&tmp, move);
        if (!is_attacked(&tmp.pos, bsf(tmp.pos.all[king_idx]), opp)) {
            legal[count++] = move;
        }
    }
    return count;
}

// --- Zstd helper ---
static void write_compressed(ZSTD_CStream* cstream, FILE* fp, const void* data, size_t size,
                             void* outBuff, size_t outBuffSize) {
    ZSTD_inBuffer input = { data, size, 0 };
    while (input.pos < input.size) {
        ZSTD_outBuffer output = { outBuff, outBuffSize, 0 };
        size_t ret = ZSTD_compressStream(cstream, &output, &input);
        if (ZSTD_isError(ret)) {
            fprintf(stderr, "ZSTD_compressStream error: %s\n", ZSTD_getErrorName(ret));
            exit(1);
        }
        dg_fwrite(outBuff, output.pos, fp);
    }
}

// --- Play one game and output compact record ---
// Every ply's move is stored (so the trainer can replay the game).
// Filtered plies get DG_SENTINEL_SCORE so the trainer can skip them during
// training but still replay the move.
//
// Returns the number of SCORED (non-sentinel) records, so the datagen target
// counts real training samples, not total plies.
static int play_game(FILE *fp, ZSTD_CStream* cstream, void* outBuff, size_t outBuffSize) {
    move_t legal[MAX_MOVES];
    GameRecord records[DG_MAX_GAME_MOVES];
    int num_records = 0;
    int num_scored = 0;   // count of real training samples

    new_game();

    // Mode-dependent minimum record ply, measured from loop-start (which is
    // after the book position and extra random plies).
    const int min_record_ply = (dg_frc || dg_dfrc) ? DG_MIN_RECORD_PLY_FRC
                                                   : DG_MIN_RECORD_PLY_STD;

    // Opening selection (with per-mode extra random plies)
    if (dg_dfrc) {
        char fen[128];
        generate_dfrc_fen(fen, sizeof(fen));
        set_position_from_fen(&nodes[0], fen);
        int extra_plies = dg_rand() % (DG_EXTRA_PLIES_DFRC + 1);
        for (int i = 0; i < extra_plies; i++) {
            int n = gen_legal_moves(legal);
            if (n == 0) break;
            move_t pick = legal[dg_rand() % n];
            apply_move(&nodes[0], pick);
            hh_push(nodes[0].pos.hash);
        }
    }
    else if (dg_frc) {
        char fen[128];
        generate_frc_fen(fen, sizeof(fen));
        set_position_from_fen(&nodes[0], fen);
        int extra_plies = dg_rand() % (DG_EXTRA_PLIES_FRC + 1);
        for (int i = 0; i < extra_plies; i++) {
            int n = gen_legal_moves(legal);
            if (n == 0) break;
            move_t pick = legal[dg_rand() % n];
            apply_move(&nodes[0], pick);
            hh_push(nodes[0].pos.hash);
        }
    }
    else if (book_count > 0) {
        set_random_book_position();
        int extra_plies = dg_rand() % (DG_EXTRA_PLIES_BOOK + 1);
        for (int i = 0; i < extra_plies; i++) {
            int n = gen_legal_moves(legal);
            if (n == 0) break;
            move_t pick = legal[dg_rand() % n];
            apply_move(&nodes[0], pick);
            hh_push(nodes[0].pos.hash);
        }
    }
    else {
        position(&nodes[0], "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR", "w", "KQkq", "-", 0, 0, NULL);
        const int span = DG_RANDOM_PLIES_MAX - DG_RANDOM_PLIES_MIN + 1;
        int num_random = DG_RANDOM_PLIES_MIN + dg_rand() % span;
        for (int i = 0; i < num_random; i++) {
            int n = gen_legal_moves(legal);
            if (n == 0) return 0;
            move_t pick = legal[dg_rand() % n];
            apply_move(&nodes[0], pick);
            hh_push(nodes[0].pos.hash);
        }
    }

    if (is_mat_draw(&nodes[0].pos))
        return 0;

    // Header board = position right before the loop starts
    Position start_pos;
    pos_copy(&nodes[0].pos, &start_pos);

    int wdl = VIRI_WDL_DRAW;
    int draw_count = 0;
    int adjudication_counter = 0;

    for (int ply = 0; ply < DG_MAX_GAME_MOVES; ply++) {
        Position *pos = &nodes[0].pos;
        const int stm = pos->stm;
        const int opp = stm ^ 1;
        const int king_idx = piece_index(KING, stm);
        const int in_check = is_attacked(pos, bsf(pos->all[king_idx]), opp);

        int n = gen_legal_moves(legal);
        if (n == 0) {
            if (in_check)
                wdl = (stm == WHITE) ? VIRI_WDL_BLACK_WIN : VIRI_WDL_WHITE_WIN;
            break;
        }

        init_tc(0, 0, 0, 0, DG_HARD_NODES, 0, 0, 0, 0);
        time_control.soft_nodes = DG_SEARCH_NODES;
        time_control.best_move = 0;
        time_control.best_score = 0;
        go(1);

        move_t best = time_control.best_move;
        int score = time_control.best_score;
        if (!best) {
            dg_fallback_moves++;
            best = legal[0];
        }

        int white_score = (stm == WHITE) ? score : -score;

        // ALWAYS record the move (so the trainer can replay the game).
        // Only assign a real score if the position passes the filter;
        // otherwise use the sentinel so the trainer skips it for training.
        if (num_records < DG_MAX_GAME_MOVES) {
            records[num_records].move = move_to_viri(best);

            // Downsample is a compile-time constant; when it's 1 we skip
            // the RNG call entirely to keep the RNG stream deterministic
            // across builds.
            #if DG_DOWNSAMPLE_RATE > 1
                int downsample_keep = ((dg_rand() % DG_DOWNSAMPLE_RATE) == 0);
            #else
                int downsample_keep = 1;
            #endif

            int keep = (ply >= min_record_ply) &&
                       !in_check &&
                       (n > 1) &&
                       downsample_keep &&
                       is_unique_and_add(pos->hash);

            if (keep) {
                records[num_records].score = (int16_t)white_score;
                num_scored++;
            } else {
                records[num_records].score = DG_SENTINEL_SCORE;
            }
            num_records++;
        }

        // Adjudication
        if (abs(score) <= DG_DRAW_SCORE)
            draw_count++;
        else
            draw_count = 0;

        if (draw_count >= DG_DRAW_COUNT && ply >= DG_DRAW_PLY) {
            wdl = VIRI_WDL_DRAW;
            break;
        }

        if (abs(score) > DG_ADJUDICATE_SCORE) {
            adjudication_counter++;
            if (adjudication_counter >= DG_ADJUDICATE_COUNT) {
                if (score > 0)
                    wdl = (stm == WHITE) ? VIRI_WDL_WHITE_WIN : VIRI_WDL_BLACK_WIN;
                else
                    wdl = (stm == WHITE) ? VIRI_WDL_BLACK_WIN : VIRI_WDL_WHITE_WIN;
                break;
            }
        } else {
            adjudication_counter = 0;
        }

        apply_move(&nodes[0], best);
        hh_push(nodes[0].pos.hash);

        hh_set_root();
        if (is_draw(0, nodes[0].pos.hash, nodes[0].pos.hmc) || is_mat_draw(&nodes[0].pos)) {
            wdl = VIRI_WDL_DRAW;
            break;
        }
    }

    // Write the entire game in one shot: header + all records.
    // Layout: start_board (32) | wdl (1) | rec_count (2) | rec[0..num_records-1] (4 each).
    // Buffering avoids ~150 ZSTD_compressStream calls per game versus the old
    // per-record write loop.
    uint8_t game_buf[35 + DG_MAX_GAME_MOVES * 4];
    size_t off = 0;

    uint8_t start_board[32];
    pos_to_packed_board(&start_pos, start_board, 0);
    memcpy(game_buf, start_board, 32);

    game_buf[32] = (uint8_t)wdl;
    uint16_t rec_count = (uint16_t)num_records;
    memcpy(game_buf + 33, &rec_count, 2);
    off = 35;

    for (int i = 0; i < num_records; i++) {
        memcpy(game_buf + off,     &records[i].move,  2);
        memcpy(game_buf + off + 2, &records[i].score, 2);
        off += 4;
    }

    write_compressed(cstream, fp, game_buf, off, outBuff, outBuffSize);

    return num_scored;
}

// --- Manifest writers ---
// Two-phase: write_manifest_config() runs after the output file opens
// successfully, write_manifest_result() appends outcome stats on exit.
static void write_manifest_config(const char *path, const char *data_file,
                                  uint64_t target_positions, const char *book_file,
                                  int min_record_ply) {
    FILE *m = fopen(path, "w");
    if (!m) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char started[64];
    strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", tm_info);

    const char *mode = dg_dfrc ? "dfrc" : (dg_frc ? "frc" : "book");
    if (!dg_dfrc && !dg_frc && book_count == 0) mode = "random";

    fprintf(m, "# Cwtch datagen run manifest\n");
    fprintf(m, "started           %s\n", started);
    fprintf(m, "pid               %d\n", (int)getpid());
    fprintf(m, "output_file       %s\n", data_file);
    fprintf(m, "mode              %s\n", mode);
    fprintf(m, "book_file         %s\n", book_file ? book_file : "(none)");
    fprintf(m, "book_positions    %d\n", book_count);
    fprintf(m, "target_positions  %llu\n", (unsigned long long)target_positions);
    fprintf(m, "search_nodes      %d (soft, hard %d)\n", DG_SEARCH_NODES, DG_HARD_NODES);
    fprintf(m, "min_record_ply    %d\n", min_record_ply);
    fprintf(m, "draw_score        %d\n", DG_DRAW_SCORE);
    fprintf(m, "draw_count        %d\n", DG_DRAW_COUNT);
    fprintf(m, "draw_ply          %d\n", DG_DRAW_PLY);
    fprintf(m, "max_game_moves    %d\n", DG_MAX_GAME_MOVES);
    fprintf(m, "adjudicate_score  %d\n", DG_ADJUDICATE_SCORE);
    fprintf(m, "adjudicate_count  %d\n", DG_ADJUDICATE_COUNT);
    fprintf(m, "extra_plies_dfrc  %d\n", DG_EXTRA_PLIES_DFRC);
    fprintf(m, "extra_plies_frc   %d\n", DG_EXTRA_PLIES_FRC);
    fprintf(m, "extra_plies_book  %d\n", DG_EXTRA_PLIES_BOOK);
    fprintf(m, "dg_seed           0x%016llx\n", (unsigned long long)dg_seed);
    fclose(m);
}

static void write_manifest_result(const char *path,
                                  uint64_t positions, uint64_t games,
                                  uint64_t wall_ms) {
    FILE *m = fopen(path, "a");
    if (!m) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char finished[64];
    strftime(finished, sizeof(finished), "%Y-%m-%d %H:%M:%S", tm_info);

    double dup_pct = dg_total_queries
        ? 100.0 * (double)dg_total_duplicates / (double)dg_total_queries
        : 0.0;

    fprintf(m, "\n## run result\n");
    fprintf(m, "finished          %s\n", finished);
    fprintf(m, "positions         %llu\n", (unsigned long long)positions);
    fprintf(m, "games             %llu\n", (unsigned long long)games);
    fprintf(m, "wall_seconds      %llu\n", (unsigned long long)(wall_ms / 1000ULL));
    fprintf(m, "write_failed      %d\n", dg_write_failed);
    fprintf(m, "dup_queries       %llu\n", (unsigned long long)dg_total_queries);
    fprintf(m, "dup_rejects       %llu\n", (unsigned long long)dg_total_duplicates);
    fprintf(m, "dup_rate_pct      %.2f\n", dup_pct);
    fprintf(m, "fallback_moves    %llu\n", (unsigned long long)dg_fallback_moves);
    fclose(m);
}

// --- Main datagen loop ---
void datagen(const char *directory, uint64_t target_positions, const char *book_file) {
    dg_seed_rng();
    init_bloom();
    new_tt(16);

    // Install signal handlers so Ctrl-C / SIGTERM triggers a clean shutdown.
    // The main loop polls dg_stop_requested once per game; on the next check
    // it breaks out and runs the normal flush/end-stream/close path.
    signal(SIGINT,  dg_handle_signal);
    signal(SIGTERM, dg_handle_signal);

    if (dg_frc || dg_dfrc)
        is_chess960 = 1;
    else
        is_chess960 = 0;

    load_book(book_file);

    const int min_record_ply = (dg_frc || dg_dfrc) ? DG_MIN_RECORD_PLY_FRC
                                                   : DG_MIN_RECORD_PLY_STD;

    char filename[512];
    snprintf(filename, sizeof(filename), "%s/" DG_FILE_PREFIX "%llu.vf.zst",
             directory, (unsigned long long)dg_rand());

    char manifest_path[520];
    snprintf(manifest_path, sizeof(manifest_path), "%s.manifest", filename);

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("error: cannot open %s\n", filename);
        free_bloom();
        return;
    }

    // Manifest is written only after we know the output file is openable.
    write_manifest_config(manifest_path, filename, target_positions,
                          book_file, min_record_ply);

    ZSTD_CStream* cstream = ZSTD_createCStream();
    if (!cstream) {
        fprintf(stderr, "ZSTD_createCStream failed\n");
        fclose(fp);
        remove(filename);
        remove(manifest_path);
        free_bloom();
        return;
    }

    size_t ret = ZSTD_initCStream(cstream, 1);
    if (ZSTD_isError(ret)) {
        fprintf(stderr, "ZSTD_initCStream error: %s\n", ZSTD_getErrorName(ret));
        ZSTD_freeCStream(cstream);
        fclose(fp);
        remove(filename);
        remove(manifest_path);
        free_bloom();
        return;
    }

    size_t outBuffSize = ZSTD_CStreamOutSize();
    void* outBuff = malloc(outBuffSize);
    if (!outBuff) {
        fprintf(stderr, "malloc failed\n");
        ZSTD_freeCStream(cstream);
        fclose(fp);
        remove(filename);
        remove(manifest_path);
        free_bloom();
        return;
    }

    fprintf(stderr, "datagen: writing compact game records to %s, target %llu positions\n",
            filename, (unsigned long long)target_positions);

    uint64_t start_time = time_ms();
    uint64_t total_positions = 0;
    uint64_t total_games = 0;
    uint64_t last_report = start_time;

    while (total_positions < target_positions) {
        // Clean shutdown check: poll once per game.
        if (dg_stop_requested) {
            fprintf(stderr, "\ndatagen: shutdown requested — flushing and closing\n");
            break;
        }

        if (dg_write_failed)
            break;

        int samples = play_game(fp, cstream, outBuff, outBuffSize);
        total_positions += samples;
        total_games++;

        // Reset the bloom filter after 50M inserts. Positions already written
        // earlier in this run may be re-accepted after a reset — this is
        // deliberate. A strict lifetime uniqueness check would require storing
        // every hash (hundreds of GB for a 500M run), which isn't worth the
        // disk/RAM cost. The rate of cross-epoch re-acceptance is negligible
        // relative to the size of the game tree.
        if (bloom_insertions >= BLOOM_RESET_INSERTS) {
            memset(bloom_filter, 0, BLOOM_SIZE_BYTES);
            bloom_insertions = 0;
            dg_duplicate_rejects = 0;
        }

        uint64_t now = time_ms();
        if (now - last_report >= DG_REPORT_SECS * 1000) {
            ZSTD_outBuffer output = { outBuff, outBuffSize, 0 };
            while (true) {
                size_t r = ZSTD_flushStream(cstream, &output);
                dg_fwrite(outBuff, output.pos, fp);
                output.pos = 0;
                if (r == 0) break;
                if (ZSTD_isError(r)) {
                    fprintf(stderr, "ZSTD_flushStream error: %s\n", ZSTD_getErrorName(r));
                    break;
                }
            }
            if (fflush(fp) != 0)
                dg_write_failed = 1;

            uint64_t elapsed = now - start_time;
            uint64_t pps = elapsed ? (total_positions * 1000ULL / elapsed) : 0;
            uint64_t remaining_pos = target_positions > total_positions ? target_positions - total_positions : 0;
            uint64_t eta_ms = pps ? (remaining_pos * 1000ULL / pps) : 0;
            double pct = target_positions ? (100.0 * total_positions / target_positions) : 0.0;

            // Epoch dup rate (correct within the current bloom generation).
            uint64_t queried = bloom_insertions + dg_duplicate_rejects;
            double dup_pct = queried
                ? (100.0 * (double)dg_duplicate_rejects / (double)queried)
                : 0.0;

            int eta_s = (int)(eta_ms / 1000ULL);
            int eta_d = eta_s / 86400;
            int eta_h = (eta_s % 86400) / 3600;
            int eta_m = (eta_s % 3600) / 60;

            char eta[32];
            if (eta_d > 0)
                snprintf(eta, sizeof eta, "%dd %02d:%02d", eta_d, eta_h, eta_m);
            else
                snprintf(eta, sizeof eta, "%d:%02d", eta_h, eta_m);

            fprintf(stderr,
                    "datagen: %llu/%llu positions (%.1f%%) %llu games %llu pos/s "
                    "dup %.1f%% fallback %llu [%s left]\n",
                    (unsigned long long)total_positions,
                    (unsigned long long)target_positions,
                    pct,
                    (unsigned long long)total_games,
                    (unsigned long long)pps,
                    dup_pct,
                    (unsigned long long)dg_fallback_moves,
                    eta);
            fflush(stderr);
            last_report = now;
        }
    }

    ZSTD_outBuffer output = { outBuff, outBuffSize, 0 };
    while (true) {
        size_t r = ZSTD_endStream(cstream, &output);
        dg_fwrite(outBuff, output.pos, fp);
        output.pos = 0;
        if (r == 0) break;
        if (ZSTD_isError(r)) {
            fprintf(stderr, "ZSTD_endStream error: %s\n", ZSTD_getErrorName(r));
            break;
        }
    }

    if (fclose(fp) != 0)
        dg_write_failed = 1;
    free(outBuff);
    ZSTD_freeCStream(cstream);
    free_book();
    free_bloom();

    uint64_t wall_ms = time_ms() - start_time;

    fprintf(stderr, "datagen: done. %llu positions %llu games written to %s\n",
            (unsigned long long)total_positions,
            (unsigned long long)total_games,
            filename);
    fprintf(stderr, "datagen: final stats — dup rejects %llu, fallback moves %llu\n",
            (unsigned long long)dg_total_duplicates,
            (unsigned long long)dg_fallback_moves);

    // Append outcome block to the manifest so the file is a complete record.
    write_manifest_result(manifest_path, total_positions, total_games, wall_ms);

    if (dg_write_failed) {
        fprintf(stderr, "datagen: FAILED, %s is incomplete\n", filename);
        exit(1);
    }
}