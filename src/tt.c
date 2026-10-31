#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/mman.h>
#endif
#include "types.h"
#include "tt.h"
#include "uci.h"
#include "history.h"
#include "corrhist.h"

// Four 16 byte entries per cache line. An entry is two 64 bit words, stored as
// (hash ^ data, data): a read that races with a write fails the hash check
// instead of returning a torn entry, so threads never need a lock.
//
// data bits: move 19 | score 16 | eval 16 | depth 7 | bound 2 | age 3 | pv 1

#define TT_ENTRIES_PER_CLUSTER 4
#define TT_AGE_CYCLE 8

typedef struct {
  _Atomic uint64_t key_xor;
  _Atomic uint64_t data;
} TTEntry;

typedef struct {
  _Alignas(64) TTEntry entries[TT_ENTRIES_PER_CLUSTER];
} TTCluster;

_Static_assert(sizeof(TTCluster) == 64, "a TT cluster must fill exactly one cache line");

static TTCluster *tt = NULL;
static size_t tt_clusters = 0;
static unsigned tt_age = 0;

static inline uint64_t pack_move(const move_t move) {
  // the promote flag is implied by the promotion piece bits
  return (uint64_t)((move & 0x7FFF) | ((move >> 16) & 0xF) << 15);
}

static inline move_t unpack_move(const uint64_t bits) {
  const move_t low = (move_t)(bits & 0x7FFF);
  const move_t flags = (move_t)((bits >> 15) & 0xF) << 16;
  return low | flags | ((low >> 12) & 7 ? MOVE_FLAG_PROMOTE : 0);
}

static inline uint64_t pack_data(const int bound, const int depth, const int score, const int eval, const move_t move, const int pv) {
  return pack_move(move)
       | (uint64_t)(uint16_t)(int16_t)score << 19
       | (uint64_t)(uint16_t)(int16_t)eval << 35
       | (uint64_t)(depth > 127 ? 127 : depth) << 51
       | (uint64_t)bound << 58
       | (uint64_t)tt_age << 60
       | (uint64_t)(pv ? 1 : 0) << 63;
}

static inline int data_depth(const uint64_t data) { return (int)((data >> 51) & 0x7F); }
static inline int data_bound(const uint64_t data) { return (int)((data >> 58) & 3); }
static inline unsigned data_age(const uint64_t data) { return (unsigned)((data >> 60) & 7); }

static inline TTCluster *cluster_for(const uint64_t hash) {
  return &tt[(size_t)(((__uint128_t)hash * tt_clusters) >> 64)];
}

static void *alloc_clusters(const size_t bytes) {
#ifdef _WIN32
  return _aligned_malloc(bytes, 64);
#else
  void *mem = NULL;
#ifdef __linux__
  const size_t align = bytes >= (2u << 20) ? (2u << 20) : 64;
#else
  const size_t align = 64;
#endif
  if (posix_memalign(&mem, align, bytes) != 0)
    return NULL;
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  madvise(mem, bytes, MADV_HUGEPAGE);
#endif
  return mem;
#endif
}

static void free_clusters(void *mem) {
#ifdef _WIN32
  _aligned_free(mem);
#else
  free(mem);
#endif
}

typedef struct {
  char *start;
  size_t bytes;
} ClearJob;

static void *clear_worker(void *arg) {
  const ClearJob *job = arg;
  memset(job->start, 0, job->bytes);
  return NULL;
}

void tt_clear(void) {

  const size_t bytes = tt_clusters * sizeof(TTCluster);
  int workers = num_threads < 256 ? num_threads : 256;

  // one thread is quicker below this; a huge table takes minutes on one thread
  if (bytes < (1ULL << 30) || workers < 2) {
    memset(tt, 0, bytes);
    return;
  }

  pthread_t threads[256];
  ClearJob jobs[256];
  const size_t slice = (bytes / (size_t)workers) & ~(size_t)63;

  for (int i = 0; i < workers; i++) {
    jobs[i].start = (char *)tt + slice * (size_t)i;
    jobs[i].bytes = i == workers - 1 ? bytes - slice * (size_t)i : slice;
    if (i > 0 && pthread_create(&threads[i], NULL, clear_worker, &jobs[i]) != 0) {
      memset(jobs[i].start, 0, jobs[i].bytes);
      threads[i] = threads[0];
      jobs[i].bytes = 0;
    }
  }

  clear_worker(&jobs[0]);

  for (int i = 1; i < workers; i++)
    if (jobs[i].bytes)
      pthread_join(threads[i], NULL);

}

int new_tt(size_t megabytes) {

  if (megabytes < 1) megabytes = 1;
  if (megabytes > TT_MAX_MB) megabytes = TT_MAX_MB;

  if (tt) {
    free_clusters(tt);
    tt = NULL;
    tt_clusters = 0;
  }

  const size_t bytes = megabytes * 1024ULL * 1024ULL;
  tt = alloc_clusters(bytes);

  if (!tt) {
    printf("info string failed to allocate %zu MB hash\n", megabytes);
    return 1;
  }

  tt_clusters = bytes / sizeof(TTCluster);
  tt_clear();
  tt_age = 0;
  return 0;

}

int put_adjusted_score(const int ply, const int score) {
  if (score < -MATEISH) return score - ply;
  else if (score > MATEISH) return score + ply;
  else return score;
}

int get_adjusted_score(const int ply, const int score) {
  if (score < -MATEISH) return score + ply;
  else if (score > MATEISH) return score - ply;
  else return score;
}

void tt_prefetch(const uint64_t hash) {
  __builtin_prefetch(cluster_for(hash));
}

int tt_probe(const uint64_t hash, TTHit *hit) {

  TTEntry *entries = cluster_for(hash)->entries;

  for (int i = 0; i < TT_ENTRIES_PER_CLUSTER; i++) {

    const uint64_t data = atomic_load_explicit(&entries[i].data, memory_order_relaxed);
    const uint64_t key_xor = atomic_load_explicit(&entries[i].key_xor, memory_order_relaxed);

    if ((key_xor ^ data) != hash || data_bound(data) == TT_NONE)
      continue;

    hit->move = unpack_move(data);
    hit->score = (int16_t)(uint16_t)(data >> 19);
    hit->eval = (int16_t)(uint16_t)(data >> 35);
    hit->depth = data_depth(data);
    hit->bound = data_bound(data);
    hit->pv = (int)(data >> 63);
    return 1;

  }

  return 0;

}

void tt_put(const uint64_t hash, const int bound, const int depth, const int score, const int eval, const move_t move, const int pv) {

  TTEntry *entries = cluster_for(hash)->entries;
  TTEntry *slot = NULL;
  int worst = INT32_MAX;
  move_t keep_move = move;

  for (int i = 0; i < TT_ENTRIES_PER_CLUSTER; i++) {

    const uint64_t data = atomic_load_explicit(&entries[i].data, memory_order_relaxed);
    const uint64_t key_xor = atomic_load_explicit(&entries[i].key_xor, memory_order_relaxed);

    if (data_bound(data) != TT_NONE && (key_xor ^ data) == hash) {

      // same position: a shallow result from this search mustn't erase a deep one
      if (bound != TT_EXACT && data_age(data) == tt_age && depth + 4 < data_depth(data))
        return;

      if (!keep_move)
        keep_move = unpack_move(data);

      slot = &entries[i];
      break;

    }

    if (data_bound(data) == TT_NONE) {
      slot = &entries[i];
      worst = INT32_MIN;
      continue;
    }

    // replace the shallowest entry, counting older searches as much shallower
    const unsigned age_gap = (TT_AGE_CYCLE + tt_age - data_age(data)) % TT_AGE_CYCLE;
    const int worth = data_depth(data) - 8 * (int)age_gap;

    if (worth < worst) {
      worst = worth;
      slot = &entries[i];
    }

  }

  const uint64_t data = pack_data(bound, depth, score, eval, keep_move, pv);
  atomic_store_explicit(&slot->key_xor, hash ^ data, memory_order_relaxed);
  atomic_store_explicit(&slot->data, data, memory_order_relaxed);

}

void tt_new_search(void) {
  tt_age = (tt_age + 1) % TT_AGE_CYCLE;
}

int tt_hashfull(void) {

  int used = 0;
  int sampled = 0;
  const size_t clusters = tt_clusters < 250 ? tt_clusters : 250;

  for (size_t c = 0; c < clusters; c++) {
    for (int i = 0; i < TT_ENTRIES_PER_CLUSTER; i++) {
      const uint64_t data = atomic_load_explicit(&tt[c].entries[i].data, memory_order_relaxed);
      sampled++;
      if (data_bound(data) != TT_NONE && data_age(data) == tt_age)
        used++;
    }
  }

  return sampled ? used * 1000 / sampled : 0;

}

void new_game(void) {
  if (!tt) new_tt(TT_DEFAULT_MB);
  tt_clear();
  tt_age = 0;
  clear_piece_to_history();
  clear_cont_history();
  clear_capture_history();
  clear_corrhist();
  clear_counter_moves();
}

int is_tt_null(void) {
  return (int)(tt == NULL);
}
