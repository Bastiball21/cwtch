#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "types.h"
#include "tt.h"
#include "history.h"
#include "corrhist.h"

// 1. The Ultimate Sequence Lock Struct
// 'volatile' prevents Clang -O3 from caching these in registers!
typedef struct {
  _Atomic uint32_t sequence;
  volatile uint64_t key;
  volatile move_t move;
  volatile int score;
  volatile int depth;
  volatile int flags;
} InternalTT;

static InternalTT *tt = NULL;
static size_t tt_entries = 0;
static size_t tt_mask    = 0;

_Thread_local TT unpacked_tt;

int new_tt(size_t megabytes) {
  if (megabytes < 1) megabytes = 1;
  if (megabytes > 262144) megabytes = 262144;

  if (tt) {
    free(tt);
    tt = NULL;
  }

  const size_t bytes = megabytes * 1024ULL * 1024ULL;
  tt_entries = bytes / sizeof(InternalTT);
  tt_entries = 1ULL << (63 - __builtin_clzll(tt_entries));
  tt_mask    = tt_entries - 1;
  tt         = calloc(tt_entries, sizeof(InternalTT));

  if (!tt) {
    printf("info string failed to allocate tt\n");
    return 1;
  }

  printf("info string tt entries %zu (%zu MB)\n", tt_entries, (tt_entries * sizeof(InternalTT)) / 1024 / 1024);
  return 0;
}

void tt_clear(void) {
  memset(tt, 0, tt_entries * sizeof(*tt));
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

void tt_put(const Position *pos, const int flags, const int depth, const int score, const move_t move) {
  const size_t idx = pos->hash & tt_mask;
  InternalTT *entry = &tt[idx];

  if (entry->flags && entry->key == pos->hash && entry->depth > depth)
    return;

  uint32_t seq = atomic_load_explicit(&entry->sequence, memory_order_acquire);
  if (seq & 1) return; // If ODD, locked. Drop write.

  // 2. Lock the entry (Make sequence ODD)
  if (!atomic_compare_exchange_weak_explicit(&entry->sequence, &seq, seq + 1, memory_order_acquire, memory_order_relaxed)) {
    return; 
  }

  // 3. HARD BARRIER: Ensure lock is held before writing
  atomic_thread_fence(memory_order_release);

  entry->key   = pos->hash;
  entry->move  = move;
  entry->score = score;
  entry->depth = depth;
  entry->flags = flags;

  // 4. HARD BARRIER: Ensure data is in RAM before unlocking
  atomic_thread_fence(memory_order_release);
  
  // 5. Unlock the entry (Make sequence EVEN)
  atomic_store_explicit(&entry->sequence, seq + 2, memory_order_release);
}

void tt_prefetch(const uint64_t hash) {
  __builtin_prefetch(&tt[hash & tt_mask]);
}

TT *tt_get(const Position *pos) {
  const size_t idx = pos->hash & tt_mask;
  InternalTT *entry = &tt[idx];

  uint32_t seq;
  int attempts = 0;

  while (attempts++ < 3) {
    seq = atomic_load_explicit(&entry->sequence, memory_order_acquire);
    if (seq & 1) continue; 

    // 6. HARD BARRIER: Prevent Clang from hoisting the reads outside the loop
    atomic_thread_fence(memory_order_acquire);

    uint64_t key = entry->key;
    move_t move  = entry->move;
    int score    = entry->score;
    int depth    = entry->depth;
    int flags    = entry->flags;

    // 7. HARD BARRIER: Ensure reads finish before checking sequence again
    atomic_thread_fence(memory_order_acquire);

    if (seq == atomic_load_explicit(&entry->sequence, memory_order_acquire)) {
      
      if (key != pos->hash) return NULL;
      if (!flags) return NULL;

      unpacked_tt.hash  = key;
      unpacked_tt.move  = move;
      unpacked_tt.score = score;
      unpacked_tt.depth = depth;
      unpacked_tt.flags = flags;

      return &unpacked_tt;
    }
  }
  return NULL; 
}

void new_game(void) {
  if (!tt) new_tt(TT_DEFAULT_MB);
  tt_clear();
  clear_piece_to_history();
  clear_cont_history();
  clear_capture_history();
  clear_corrhist();
  clear_counter_moves(); 
}

int is_tt_null() {
  return (int)(tt == NULL);
}