#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <cpuid.h>
#endif
#ifdef __linux__
#include <sys/mman.h>
#endif
#include "net.h"
#include "builtins.h"

#define INCBIN_PREFIX cwtch_
#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#include "incbin.h"
INCBIN(weights, NET_WEIGHTS_PATH);

#define NET_BUCKET_STRIDE (NET_I_SIZE * NET_H1_SIZE)
#define NET_L0_SIZE (NET_I_BUCKETS * NET_BUCKET_STRIDE)

// bullet pads quantised.bin to the next 64 byte boundary
#define NET_EXACT_BYTES ((size_t)(NET_L0_SIZE + NET_H1_SIZE + NET_O_BUCKETS * NET_H1_SIZE * 2 + NET_O_BUCKETS) * sizeof(int16_t))
#define NET_FILE_BYTES ((NET_EXACT_BYTES + 63) & ~(size_t)63)

// output bucket = (piece count - 2) / NET_O_DIV, matching bullet MaterialCount
#define NET_O_DIV ((32 + NET_O_BUCKETS - 1) / NET_O_BUCKETS)

// At NET_H1_SIZE=1536 and 16 buckets, net_h1_w is 36 MiB (exceeding a 32 MiB L3).
// Fast inference relies on incremental accumulator updates (2-4 rows of 3 KiB per move)
// and the Finny table below to refresh king-bucket transitions via bitboard diffs.
// Accumulator updates touch this table all over, so 4 KB pages would miss the TLB
// constantly: aligned for huge pages, requested in unpack_weights().
static _Alignas(1 << 21) int16_t net_h1_w[NET_L0_SIZE];
static _Alignas(64) int16_t net_h1_b[NET_H1_SIZE];
static _Alignas(64) int16_t net_o_w [NET_O_BUCKETS * NET_H1_SIZE * 2];
static int32_t net_o_b [NET_O_BUCKETS];

// king bucket layout in white pov; mirrored nets use files a-d only
// matches main.rs BUCKET_LAYOUT = [0,0,1,1, 2,2,3,3, ... 14,14,15,15]
static const uint8_t net_bucket_map[64] = {
  0,  0,  1,  1,  1,  1,  0,  0,   // rank 1
  2,  2,  3,  3,  3,  3,  2,  2,   // rank 2
  4,  4,  5,  5,  5,  5,  4,  4,   // rank 3
  6,  6,  7,  7,  7,  7,  6,  6,   // rank 4
  8,  8,  9,  9,  9,  9,  8,  8,   // rank 5
  10, 10, 11, 11, 11, 11, 10, 10,  // rank 6
  12, 12, 13, 13, 13, 13, 12, 12,  // rank 7
  14, 14, 15, 15, 15, 15, 14, 14,  // rank 8
};

// finny cache - one accumulator per view, refreshed by board diff
typedef struct {
  _Alignas(64) int16_t acc[NET_H1_SIZE];
  uint64_t all[12];
} FinnyEntry;

static _Thread_local FinnyEntry finny[2][NET_I_BUCKETS][2];

// per-perspective view of the feature space
typedef struct {
  int hm;
  int bucket;
  int off;
} NetView;

static inline int net_hm(const int sq) {
  return NET_MIRRORED && (sq & 4) ? 7 : 0;
}

static inline int net_bucket_off(const int sq, const int hm) {
  return net_bucket_map[sq ^ hm] * NET_BUCKET_STRIDE;
}

static inline void get_views(const Position *pos, NetView v[2]) {
  const int wksq = bsf(pos->all[WKING]);
  const int bksq = bsf(pos->all[BKING]) ^ 56;
  v[0].hm = net_hm(wksq);
  v[0].bucket = net_bucket_map[wksq ^ v[0].hm];
  v[0].off = v[0].bucket * NET_BUCKET_STRIDE;
  v[1].hm = net_hm(bksq);
  v[1].bucket = net_bucket_map[bksq ^ v[1].hm];
  v[1].off = v[1].bucket * NET_BUCKET_STRIDE;
}

static inline int net_flip_piece(const int piece) {
  return piece < 6 ? piece + 6 : piece - 6;
}

static inline const int16_t *net_row_w(const int piece, const int sq, const NetView *v) {
  return &net_h1_w[v->off + net_base(piece, sq ^ v->hm)];
}

static inline const int16_t *net_row_b(const int piece, const int sq, const NetView *v) {
  return &net_h1_w[v->off + net_base(net_flip_piece(piece), sq ^ 56 ^ v->hm)];
}

static inline const int16_t *net_row_p(const int p, const int piece, const int sq, const NetView *v) {
  return p == 0 ? net_row_w(piece, sq, v) : net_row_b(piece, sq, v);
}

// clipped squared relu: clamp(x, 0, NET_QA)^2
static inline int32_t screlu_scalar(const int16_t x) {
  const int32_t y = x < 0 ? 0 : (x > NET_QA ? NET_QA : x);
  return y * y;
}

// ===========================================================================
// Multi-versioned Accumulator Update & Rebuild Kernels
// ===========================================================================
#define DEFINE_ACC_KERNELS(SUFFIX, TARGET_ATTR) \
TARGET_ATTR static void net_move_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE], const NetView v[2]) { \
  const uint8_t *const a = node->net_deferred.args; \
  const int16_t *const restrict s1 = src[0]; \
  const int16_t *const restrict s2 = src[1]; \
  int16_t *const restrict a1 = node->accs[0]; \
  int16_t *const restrict a2 = node->accs[1]; \
  const int16_t *const restrict w1_b1 = net_row_w(a[0], a[1], &v[0]); \
  const int16_t *const restrict w1_b2 = net_row_w(a[0], a[2], &v[0]); \
  const int16_t *const restrict w2_b1 = net_row_b(a[0], a[1], &v[1]); \
  const int16_t *const restrict w2_b2 = net_row_b(a[0], a[2], &v[1]); \
  for (int i=0; i < NET_H1_SIZE; i++) { \
    a1[i] = s1[i] + w1_b2[i] - w1_b1[i]; \
    a2[i] = s2[i] + w2_b2[i] - w2_b1[i]; \
  } \
} \
TARGET_ATTR static void net_capture_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE], const NetView v[2]) { \
  const uint8_t *const a = node->net_deferred.args; \
  const int16_t *const restrict s1 = src[0]; \
  const int16_t *const restrict s2 = src[1]; \
  int16_t *const restrict a1 = node->accs[0]; \
  int16_t *const restrict a2 = node->accs[1]; \
  const int16_t *const restrict w1_b1 = net_row_w(a[0], a[1], &v[0]); \
  const int16_t *const restrict w1_b2 = net_row_w(a[2], a[3], &v[0]); \
  const int16_t *const restrict w1_b3 = net_row_w(a[0], a[3], &v[0]); \
  const int16_t *const restrict w2_b1 = net_row_b(a[0], a[1], &v[1]); \
  const int16_t *const restrict w2_b2 = net_row_b(a[2], a[3], &v[1]); \
  const int16_t *const restrict w2_b3 = net_row_b(a[0], a[3], &v[1]); \
  for (int i=0; i < NET_H1_SIZE; i++) { \
    a1[i] = s1[i] + w1_b3[i] - w1_b2[i] - w1_b1[i]; \
    a2[i] = s2[i] + w2_b3[i] - w2_b2[i] - w2_b1[i]; \
  } \
} \
TARGET_ATTR static void net_ep_capture_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE], const NetView v[2]) { \
  const uint8_t *const a = node->net_deferred.args; \
  const int16_t *const restrict s1 = src[0]; \
  const int16_t *const restrict s2 = src[1]; \
  int16_t *const restrict a1 = node->accs[0]; \
  int16_t *const restrict a2 = node->accs[1]; \
  const int16_t *const restrict w1_b1 = net_row_w(a[0], a[1], &v[0]); \
  const int16_t *const restrict w1_b2 = net_row_w(a[0], a[2], &v[0]); \
  const int16_t *const restrict w1_b3 = net_row_w(a[3], a[4], &v[0]); \
  const int16_t *const restrict w2_b1 = net_row_b(a[0], a[1], &v[1]); \
  const int16_t *const restrict w2_b2 = net_row_b(a[0], a[2], &v[1]); \
  const int16_t *const restrict w2_b3 = net_row_b(a[3], a[4], &v[1]); \
  for (int i=0; i < NET_H1_SIZE; i++) { \
    a1[i] = s1[i] + w1_b2[i] - w1_b1[i] - w1_b3[i]; \
    a2[i] = s2[i] + w2_b2[i] - w2_b1[i] - w2_b3[i]; \
  } \
} \
TARGET_ATTR static void net_castle_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE], const NetView v[2]) { \
  const uint8_t *const a = node->net_deferred.args; \
  const int16_t *const restrict s1 = src[0]; \
  const int16_t *const restrict s2 = src[1]; \
  int16_t *const restrict a1 = node->accs[0]; \
  int16_t *const restrict a2 = node->accs[1]; \
  const int16_t *const restrict w1_b1 = net_row_w(a[0], a[1], &v[0]); \
  const int16_t *const restrict w1_b2 = net_row_w(a[0], a[2], &v[0]); \
  const int16_t *const restrict w1_b3 = net_row_w(a[3], a[4], &v[0]); \
  const int16_t *const restrict w1_b4 = net_row_w(a[3], a[5], &v[0]); \
  const int16_t *const restrict w2_b1 = net_row_b(a[0], a[1], &v[1]); \
  const int16_t *const restrict w2_b2 = net_row_b(a[0], a[2], &v[1]); \
  const int16_t *const restrict w2_b3 = net_row_b(a[3], a[4], &v[1]); \
  const int16_t *const restrict w2_b4 = net_row_b(a[3], a[5], &v[1]); \
  for (int i=0; i < NET_H1_SIZE; i++) { \
    a1[i] = s1[i] + w1_b2[i] - w1_b1[i] + w1_b4[i] - w1_b3[i]; \
    a2[i] = s2[i] + w2_b2[i] - w2_b1[i] + w2_b4[i] - w2_b3[i]; \
  } \
} \
TARGET_ATTR static void net_promo_push_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE], const NetView v[2]) { \
  const uint8_t *const a = node->net_deferred.args; \
  const int16_t *const restrict s1 = src[0]; \
  const int16_t *const restrict s2 = src[1]; \
  int16_t *const restrict a1 = node->accs[0]; \
  int16_t *const restrict a2 = node->accs[1]; \
  const int16_t *const restrict w1_b1 = net_row_w(a[0], a[1], &v[0]); \
  const int16_t *const restrict w1_b2 = net_row_w(a[3], a[2], &v[0]); \
  const int16_t *const restrict w2_b1 = net_row_b(a[0], a[1], &v[1]); \
  const int16_t *const restrict w2_b2 = net_row_b(a[3], a[2], &v[1]); \
  for (int i=0; i < NET_H1_SIZE; i++) { \
    a1[i] = s1[i] + w1_b2[i] - w1_b1[i]; \
    a2[i] = s2[i] + w2_b2[i] - w2_b1[i]; \
  } \
} \
TARGET_ATTR static void net_promo_capture_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE], const NetView v[2]) { \
  const uint8_t *const a = node->net_deferred.args; \
  const int16_t *const restrict s1 = src[0]; \
  const int16_t *const restrict s2 = src[1]; \
  int16_t *const restrict a1 = node->accs[0]; \
  int16_t *const restrict a2 = node->accs[1]; \
  const int16_t *const restrict w1_b1 = net_row_w(a[0], a[1], &v[0]); \
  const int16_t *const restrict w1_b2 = net_row_w(a[4], a[2], &v[0]); \
  const int16_t *const restrict w1_b3 = net_row_w(a[3], a[2], &v[0]); \
  const int16_t *const restrict w2_b1 = net_row_b(a[0], a[1], &v[1]); \
  const int16_t *const restrict w2_b2 = net_row_b(a[4], a[2], &v[1]); \
  const int16_t *const restrict w2_b3 = net_row_b(a[3], a[2], &v[1]); \
  for (int i=0; i < NET_H1_SIZE; i++) { \
    a1[i] = s1[i] + w1_b2[i] - w1_b1[i] - w1_b3[i]; \
    a2[i] = s2[i] + w2_b2[i] - w2_b1[i] - w2_b3[i]; \
  } \
} \
TARGET_ATTR static void net_rebuild_acc_##SUFFIX(Node *node, const int p, const NetView *v) { \
  int16_t *const restrict acc = node->accs[p]; \
  const uint8_t *const board = node->pos.board; \
  memcpy(acc, net_h1_b, sizeof net_h1_b); \
  for (int sq=0; sq < 64; sq++) { \
    const int piece = board[sq]; \
    if (piece == EMPTY) continue; \
    const int16_t *const restrict w = net_row_p(p, piece, sq, v); \
    for (int h=0; h < NET_H1_SIZE; h++) acc[h] += w[h]; \
  } \
} \
TARGET_ATTR static void net_refresh_acc_##SUFFIX(Node *node, const int p, const NetView *v) { \
  FinnyEntry *const e = &finny[p][v->bucket][v->hm ? 1 : 0]; \
  const uint64_t *const all = node->pos.all; \
  int16_t *const restrict acc = e->acc; \
  for (int piece=0; piece < 12; piece++) { \
    uint64_t add = all[piece] & ~e->all[piece]; \
    uint64_t sub = e->all[piece] & ~all[piece]; \
    while (add) { \
      const int16_t *const restrict w = net_row_p(p, piece, bsf(add), v); \
      for (int h=0; h < NET_H1_SIZE; h++) acc[h] += w[h]; \
      add &= add - 1; \
    } \
    while (sub) { \
      const int16_t *const restrict w = net_row_p(p, piece, bsf(sub), v); \
      for (int h=0; h < NET_H1_SIZE; h++) acc[h] -= w[h]; \
      sub &= sub - 1; \
    } \
    e->all[piece] = all[piece]; \
  } \
  memcpy(node->accs[p], e->acc, sizeof e->acc); \
} \
TARGET_ATTR static void net_delta_acc_##SUFFIX(Node *node, const int16_t *const restrict src, const int p, const NetView *v) { \
  const uint8_t *const a = node->net_deferred.args; \
  int16_t *const restrict acc = node->accs[p]; \
  switch (node->net_deferred.type) { \
    case NET_OP_MOVE: { \
      const int16_t *const restrict w_b1 = net_row_p(p, a[0], a[1], v); \
      const int16_t *const restrict w_b2 = net_row_p(p, a[0], a[2], v); \
      for (int i=0; i < NET_H1_SIZE; i++) acc[i] = src[i] + w_b2[i] - w_b1[i]; \
      break; \
    } \
    case NET_OP_CAPTURE: { \
      const int16_t *const restrict w_b1 = net_row_p(p, a[0], a[1], v); \
      const int16_t *const restrict w_b2 = net_row_p(p, a[2], a[3], v); \
      const int16_t *const restrict w_b3 = net_row_p(p, a[0], a[3], v); \
      for (int i=0; i < NET_H1_SIZE; i++) acc[i] = src[i] + w_b3[i] - w_b2[i] - w_b1[i]; \
      break; \
    } \
    case NET_OP_CASTLE: { \
      const int16_t *const restrict w_b1 = net_row_p(p, a[0], a[1], v); \
      const int16_t *const restrict w_b2 = net_row_p(p, a[0], a[2], v); \
      const int16_t *const restrict w_b3 = net_row_p(p, a[3], a[4], v); \
      const int16_t *const restrict w_b4 = net_row_p(p, a[3], a[5], v); \
      for (int i=0; i < NET_H1_SIZE; i++) acc[i] = src[i] + w_b2[i] - w_b1[i] + w_b4[i] - w_b3[i]; \
      break; \
    } \
    default: \
      printf("net_delta_acc bad op %d\n", node->net_deferred.type); \
      exit(1); \
  } \
} \
TARGET_ATTR static void update_accs_##SUFFIX(Node *node, const int16_t (*src)[NET_H1_SIZE]) { \
  NetView v[2]; \
  get_views(&node->pos, v); \
  const uint8_t *const a = node->net_deferred.args; \
  if (piece_type(a[0]) == KING) { \
    const int p = piece_colour(a[0]); \
    const int from = p == WHITE ? a[1] : a[1] ^ 56; \
    const int hm = net_hm(from); \
    if (hm != v[p].hm || net_bucket_off(from, hm) != v[p].off) { \
      net_refresh_acc_##SUFFIX(node, p, &v[p]); \
      net_delta_acc_##SUFFIX(node, src[p ^ 1], p ^ 1, &v[p ^ 1]); \
      return; \
    } \
  } \
  switch (node->net_deferred.type) { \
    case NET_OP_MOVE: net_move_##SUFFIX(node, src, v); break; \
    case NET_OP_CAPTURE: net_capture_##SUFFIX(node, src, v); break; \
    case NET_OP_EP_CAPTURE: net_ep_capture_##SUFFIX(node, src, v); break; \
    case NET_OP_CASTLE: net_castle_##SUFFIX(node, src, v); break; \
    case NET_OP_PROMO_PUSH: net_promo_push_##SUFFIX(node, src, v); break; \
    case NET_OP_PROMO_CAPTURE: net_promo_capture_##SUFFIX(node, src, v); break; \
  } \
} \
TARGET_ATTR static void net_slow_rebuild_accs_##SUFFIX(Node *node) { \
  NetView v[2]; \
  get_views(&node->pos, v); \
  net_rebuild_acc_##SUFFIX(node, 0, &v[0]); \
  net_rebuild_acc_##SUFFIX(node, 1, &v[1]); \
}

// Baseline scalar / SSE2 kernels
DEFINE_ACC_KERNELS(scalar, )

static int net_eval_scalar(Node *node) {
  const int stm = node->pos.stm;
  const int ob = (popcount(node->pos.occupied) - 2) / NET_O_DIV;

  const int16_t *const restrict a1 = (stm == 0 ? node->accs[0] : node->accs[1]);
  const int16_t *const restrict a2 = (stm == 0 ? node->accs[1] : node->accs[0]);

  const int16_t *const restrict w1 = &net_o_w[ob * NET_H1_SIZE * 2];
  const int16_t *const restrict w2 = w1 + NET_H1_SIZE;

  int64_t acc = 0;
  for (int i = 0; i < NET_H1_SIZE; i++) {
    acc += (int64_t)w1[i] * screlu_scalar(a1[i]) + (int64_t)w2[i] * screlu_scalar(a2[i]);
  }

  acc /= NET_QA;
  acc += net_o_b[ob];
  acc *= NET_SCALE;
  acc /= NET_QAB;

  return (int)acc;
}

#if defined(__x86_64__) || defined(_M_X64)
#define TARGET_AVX2       __attribute__((target("avx2,bmi,bmi2,lzcnt,popcnt")))
#define TARGET_AVX512BW   __attribute__((target("avx512f,avx512bw,avx512dq,avx512vl,bmi,bmi2,lzcnt,popcnt")))
#define TARGET_AVX512VNNI __attribute__((target("avx512f,avx512bw,avx512dq,avx512vl,avx512vnni,bmi,bmi2,lzcnt,popcnt")))

DEFINE_ACC_KERNELS(avx2, TARGET_AVX2)
DEFINE_ACC_KERNELS(avx512, TARGET_AVX512BW)

TARGET_AVX2 static int net_eval_avx2(Node *node) {
  const int stm = node->pos.stm;
  const int ob = (popcount(node->pos.occupied) - 2) / NET_O_DIV;

  const int16_t *const restrict a1 = (stm == 0 ? node->accs[0] : node->accs[1]);
  const int16_t *const restrict a2 = (stm == 0 ? node->accs[1] : node->accs[0]);

  const int16_t *const restrict w1 = &net_o_w[ob * NET_H1_SIZE * 2];
  const int16_t *const restrict w2 = w1 + NET_H1_SIZE;

  const __m256i zero = _mm256_setzero_si256();
  const __m256i qa   = _mm256_set1_epi16(NET_QA);
  __m256i sum1 = _mm256_setzero_si256();
  __m256i sum2 = _mm256_setzero_si256();

  for (int i = 0; i < NET_H1_SIZE; i += 16) {
    __m256i v1 = _mm256_load_si256((const __m256i *)&a1[i]);
    __m256i v2 = _mm256_load_si256((const __m256i *)&a2[i]);
    const __m256i u1 = _mm256_load_si256((const __m256i *)&w1[i]);
    const __m256i u2 = _mm256_load_si256((const __m256i *)&w2[i]);

    v1 = _mm256_min_epi16(_mm256_max_epi16(v1, zero), qa);
    v2 = _mm256_min_epi16(_mm256_max_epi16(v2, zero), qa);

    const __m256i vw1 = _mm256_mullo_epi16(v1, u1);
    const __m256i vw2 = _mm256_mullo_epi16(v2, u2);

    sum1 = _mm256_add_epi32(sum1, _mm256_madd_epi16(vw1, v1));
    sum2 = _mm256_add_epi32(sum2, _mm256_madd_epi16(vw2, v2));
  }

  int32_t lanes1[8], lanes2[8];
  _mm256_storeu_si256((__m256i *)lanes1, sum1);
  _mm256_storeu_si256((__m256i *)lanes2, sum2);
  int32_t s1 = 0, s2 = 0;
  for (int j = 0; j < 8; j++) {
    s1 += lanes1[j];
    s2 += lanes2[j];
  }
  int64_t acc = (int64_t)s1 + (int64_t)s2;

  acc /= NET_QA;
  acc += net_o_b[ob];
  acc *= NET_SCALE;
  acc /= NET_QAB;

  return (int)acc;
}

TARGET_AVX512BW static int net_eval_avx512bw(Node *node) {
  const int stm = node->pos.stm;
  const int ob = (popcount(node->pos.occupied) - 2) / NET_O_DIV;

  const int16_t *const restrict a1 = (stm == 0 ? node->accs[0] : node->accs[1]);
  const int16_t *const restrict a2 = (stm == 0 ? node->accs[1] : node->accs[0]);

  const int16_t *const restrict w1 = &net_o_w[ob * NET_H1_SIZE * 2];
  const int16_t *const restrict w2 = w1 + NET_H1_SIZE;

  const __m512i zero = _mm512_setzero_si512();
  const __m512i qa   = _mm512_set1_epi16(NET_QA);
  __m512i sum1 = _mm512_setzero_si512();
  __m512i sum2 = _mm512_setzero_si512();

  for (int i = 0; i < NET_H1_SIZE; i += 32) {
    __m512i v1 = _mm512_load_si512((const __m512i *)&a1[i]);
    __m512i v2 = _mm512_load_si512((const __m512i *)&a2[i]);
    const __m512i u1 = _mm512_load_si512((const __m512i *)&w1[i]);
    const __m512i u2 = _mm512_load_si512((const __m512i *)&w2[i]);

    v1 = _mm512_min_epi16(_mm512_max_epi16(v1, zero), qa);
    v2 = _mm512_min_epi16(_mm512_max_epi16(v2, zero), qa);

    // |u| <= 127 and 0 <= v <= 255 => v * u in [-32385, 32385], exact in int16
    const __m512i vw1 = _mm512_mullo_epi16(v1, u1);
    const __m512i vw2 = _mm512_mullo_epi16(v2, u2);

    sum1 = _mm512_add_epi32(sum1, _mm512_madd_epi16(vw1, v1));
    sum2 = _mm512_add_epi32(sum2, _mm512_madd_epi16(vw2, v2));
  }

  int64_t acc = (int64_t)_mm512_reduce_add_epi32(sum1) + (int64_t)_mm512_reduce_add_epi32(sum2);

  acc /= NET_QA;
  acc += net_o_b[ob];
  acc *= NET_SCALE;
  acc /= NET_QAB;

  return (int)acc;
}

TARGET_AVX512VNNI static int net_eval_avx512_vnni(Node *node) {
  const int stm = node->pos.stm;
  const int ob = (popcount(node->pos.occupied) - 2) / NET_O_DIV;

  const int16_t *const restrict a1 = (stm == 0 ? node->accs[0] : node->accs[1]);
  const int16_t *const restrict a2 = (stm == 0 ? node->accs[1] : node->accs[0]);

  const int16_t *const restrict w1 = &net_o_w[ob * NET_H1_SIZE * 2];
  const int16_t *const restrict w2 = w1 + NET_H1_SIZE;

  const __m512i zero = _mm512_setzero_si512();
  const __m512i qa   = _mm512_set1_epi16(NET_QA);
  __m512i sum1 = _mm512_setzero_si512();
  __m512i sum2 = _mm512_setzero_si512();

  for (int i = 0; i < NET_H1_SIZE; i += 32) {
    __m512i v1 = _mm512_load_si512((const __m512i *)&a1[i]);
    __m512i v2 = _mm512_load_si512((const __m512i *)&a2[i]);
    const __m512i u1 = _mm512_load_si512((const __m512i *)&w1[i]);
    const __m512i u2 = _mm512_load_si512((const __m512i *)&w2[i]);

    v1 = _mm512_min_epi16(_mm512_max_epi16(v1, zero), qa);
    v2 = _mm512_min_epi16(_mm512_max_epi16(v2, zero), qa);

    // |u| <= 127 and 0 <= v <= 255 => v * u in [-32385, 32385], exact in int16
    const __m512i vw1 = _mm512_mullo_epi16(v1, u1);
    const __m512i vw2 = _mm512_mullo_epi16(v2, u2);

    sum1 = _mm512_dpwssd_epi32(sum1, vw1, v1);
    sum2 = _mm512_dpwssd_epi32(sum2, vw2, v2);
  }

  int64_t acc = (int64_t)_mm512_reduce_add_epi32(sum1) + (int64_t)_mm512_reduce_add_epi32(sum2);

  acc /= NET_QA;
  acc += net_o_b[ob];
  acc *= NET_SCALE;
  acc /= NET_QAB;

  return (int)acc;
}
#endif

// ===========================================================================
// Runtime SIMD Dispatch Table
// ===========================================================================
typedef int  (*net_eval_fn_t)(Node *node);
typedef void (*update_accs_fn_t)(Node *node, const int16_t (*src)[NET_H1_SIZE]);
typedef void (*rebuild_accs_fn_t)(Node *node);

static net_eval_fn_t     active_net_eval          = net_eval_scalar;
static update_accs_fn_t  active_update_accs       = update_accs_scalar;
static rebuild_accs_fn_t active_slow_rebuild_accs = net_slow_rebuild_accs_scalar;
static const char       *active_simd_name         = "scalar";
static char              detected_cpu_brand[64]   = "Unknown CPU";

static void detect_cpu_brand(void) {
#if defined(__x86_64__) || defined(_M_X64)
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  __get_cpuid(0x80000000U, &eax, &ebx, &ecx, &edx);
  if (eax >= 0x80000004U) {
    unsigned int regs[12];
    __get_cpuid(0x80000002U, &regs[0], &regs[1], &regs[2], &regs[3]);
    __get_cpuid(0x80000003U, &regs[4], &regs[5], &regs[6], &regs[7]);
    __get_cpuid(0x80000004U, &regs[8], &regs[9], &regs[10], &regs[11]);
    char raw[49];
    memcpy(raw, regs, 48);
    raw[48] = '\0';
    // Trim leading spaces
    const char *p = raw;
    while (*p == ' ') p++;
    // Trim trailing spaces
    snprintf(detected_cpu_brand, sizeof(detected_cpu_brand), "%s", p);
    size_t len = strlen(detected_cpu_brand);
    while (len > 0 && detected_cpu_brand[len - 1] == ' ') {
      detected_cpu_brand[--len] = '\0';
    }
    if (len > 0) return;
  }
#elif defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      if (strncasecmp(line, "model name", 10) == 0 ||
          strncasecmp(line, "Processor", 9) == 0 ||
          strncasecmp(line, "Hardware", 8) == 0) {
        char *colon = strchr(line, ':');
        if (colon) {
          colon++;
          while (*colon == ' ' || *colon == '\t') colon++;
          size_t len = strlen(colon);
          while (len > 0 && (colon[len - 1] == '\n' || colon[len - 1] == '\r' || colon[len - 1] == ' ')) {
            colon[--len] = '\0';
          }
          if (len > 0) {
            snprintf(detected_cpu_brand, sizeof(detected_cpu_brand), "%s", colon);
            fclose(f);
            return;
          }
        }
      }
    }
    fclose(f);
  }
#endif
  snprintf(detected_cpu_brand, sizeof(detected_cpu_brand), "%s", net_arch_name());
}

const char *net_os_name(void) {
#if defined(_WIN64)
  return "Windows (64-bit)";
#elif defined(_WIN32)
  return "Windows (32-bit)";
#elif defined(__ANDROID__)
  return "Android";
#elif defined(__linux__)
  return "Linux";
#elif defined(__APPLE__) && defined(__MACH__)
  return "macOS";
#elif defined(__FreeBSD__)
  return "FreeBSD";
#else
  return "Unknown OS";
#endif
}

const char *net_arch_name(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm32";
#else
  return "unknown_arch";
#endif
}

const char *net_cpu_brand(void) {
  return detected_cpu_brand;
}

const char *net_simd_name(void) {
  return active_simd_name;
}

int net_set_simd(const char *name) {
  if (!name || strcasecmp(name, "auto") == 0) {
#if defined(__x86_64__) || defined(_M_X64)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vl") &&
        __builtin_cpu_supports("avx512vnni")) {
      active_net_eval          = net_eval_avx512_vnni;
      active_update_accs       = update_accs_avx512;
      active_slow_rebuild_accs = net_slow_rebuild_accs_avx512;
      active_simd_name         = "avx512_vnni";
      return 0;
    }
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vl")) {
      active_net_eval          = net_eval_avx512bw;
      active_update_accs       = update_accs_avx512;
      active_slow_rebuild_accs = net_slow_rebuild_accs_avx512;
      active_simd_name         = "avx512bw";
      return 0;
    }
    if (__builtin_cpu_supports("avx2")) {
      active_net_eval          = net_eval_avx2;
      active_update_accs       = update_accs_avx2;
      active_slow_rebuild_accs = net_slow_rebuild_accs_avx2;
      active_simd_name         = "avx2";
      return 0;
    }
#endif
    active_net_eval          = net_eval_scalar;
    active_update_accs       = update_accs_scalar;
    active_slow_rebuild_accs = net_slow_rebuild_accs_scalar;
    active_simd_name         = "scalar";
    return 0;
  }

  if (strcasecmp(name, "scalar") == 0) {
    active_net_eval          = net_eval_scalar;
    active_update_accs       = update_accs_scalar;
    active_slow_rebuild_accs = net_slow_rebuild_accs_scalar;
    active_simd_name         = "scalar";
    return 0;
  }

#if defined(__x86_64__) || defined(_M_X64)
  __builtin_cpu_init();
  if (strcasecmp(name, "avx2") == 0) {
    if (!__builtin_cpu_supports("avx2")) return -1;
    active_net_eval          = net_eval_avx2;
    active_update_accs       = update_accs_avx2;
    active_slow_rebuild_accs = net_slow_rebuild_accs_avx2;
    active_simd_name         = "avx2";
    return 0;
  }
  if (strcasecmp(name, "avx512bw") == 0) {
    if (!(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl"))) return -1;
    active_net_eval          = net_eval_avx512bw;
    active_update_accs       = update_accs_avx512;
    active_slow_rebuild_accs = net_slow_rebuild_accs_avx512;
    active_simd_name         = "avx512bw";
    return 0;
  }
  if (strcasecmp(name, "avx512_vnni") == 0 || strcasecmp(name, "avx512vnni") == 0) {
    if (!(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl") && __builtin_cpu_supports("avx512vnni"))) return -1;
    active_net_eval          = net_eval_avx512_vnni;
    active_update_accs       = update_accs_avx512;
    active_slow_rebuild_accs = net_slow_rebuild_accs_avx512;
    active_simd_name         = "avx512_vnni";
    return 0;
  }
#endif
  return -1;
}

void net_init_thread(void) {
  for (int p=0; p < 2; p++)
    for (int b=0; b < NET_I_BUCKETS; b++)
      for (int m=0; m < 2; m++) {
        memcpy(finny[p][b][m].acc, net_h1_b, sizeof net_h1_b);
        memset(finny[p][b][m].all, 0, sizeof finny[p][b][m].all);
      }
}

static void unpack_weights(const int16_t *weights) {
  size_t offset = 0;

#if defined(__linux__) && defined(MADV_HUGEPAGE)
  madvise(net_h1_w, sizeof(net_h1_w), MADV_HUGEPAGE);
#endif

  for (int i=0; i < NET_L0_SIZE; i++) net_h1_w[i] = weights[i];
  offset += NET_L0_SIZE;

  for (int i=0; i < NET_H1_SIZE; i++) net_h1_b[i] = weights[offset+i];
  offset += NET_H1_SIZE;

  for (int i=0; i < NET_O_BUCKETS * NET_H1_SIZE * 2; i++) {
    const int16_t w = weights[offset+i];
    if (w < -127 || w > 127) {
      fprintf(stderr, "fatal: net_o_w[%d] = %d exceeds [-127, 127] int16 madd limit\n", i, (int)w);
      exit(1);
    }
    net_o_w[i] = w;
  }
  offset += NET_O_BUCKETS * NET_H1_SIZE * 2;

  for (int i=0; i < NET_O_BUCKETS; i++)
    net_o_b[i] = (int32_t)weights[offset+i];

  for (int p=0; p < 2; p++)
    for (int b=0; b < NET_I_BUCKETS; b++)
      for (int m=0; m < 2; m++) {
        memcpy(finny[p][b][m].acc, net_h1_b, sizeof net_h1_b);
        memset(finny[p][b][m].all, 0, sizeof finny[p][b][m].all);
      }
}

int init_weights(void) {
  detect_cpu_brand();
  const char *env_simd = getenv("CWTCH_SIMD");
  if (net_set_simd(env_simd ? env_simd : "auto") != 0) {
    net_set_simd("auto");
  }

  size_t bytes = (size_t)cwtch_weights_size;
  if (bytes != NET_FILE_BYTES) {
    fprintf(stderr, "fatal: embedded net is %llu bytes, this build needs %llu (H1=%d, input buckets=%d)\n",
            (unsigned long long)bytes, (unsigned long long)NET_FILE_BYTES, NET_H1_SIZE, NET_I_BUCKETS);
    return 1;
  }
  unpack_weights((const int16_t *)cwtch_weights_data);
  return 0;
}

int load_weights_from_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { printf("info string cannot open %s\n", path); return 1; }
  fseek(f, 0, SEEK_END);
  long bytes = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (bytes <= 0 || (size_t)bytes != NET_FILE_BYTES) {
    printf("info string %s is %llu bytes, expected %llu\n", path,
           (unsigned long long)bytes, (unsigned long long)NET_FILE_BYTES);
    fclose(f);
    return 1;
  }
  int16_t *buf = (int16_t *)malloc(bytes);
  if (!buf) { printf("info string allocation failed\n"); fclose(f); return 1; }
  if (fread(buf, 1, bytes, f) != (size_t)bytes) {
    printf("info string read error\n"); free(buf); fclose(f); return 1;
  }
  fclose(f);
  unpack_weights(buf);
  free(buf);
  net_slow_rebuild_accs(&nodes[0]);
  nodes[0].accs_dirty = 0;
  printf("info string loaded %s\n", path);
  return 0;
}

void lazy_update_accs(Node *node) {
  if (!node->accs_dirty) return;
  active_update_accs(node, (node - 1)->accs);
  node->accs_dirty = 0;
}

void update_accs(Node *node, const int16_t (*src)[NET_H1_SIZE]) {
  active_update_accs(node, src);
}

void net_slow_rebuild_accs(Node *node) {
  active_slow_rebuild_accs(node);
}

int net_eval(Node *node) {
  return active_net_eval(node);
}
