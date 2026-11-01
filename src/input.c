#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "timecontrol.h"

// A dedicated thread reads stdin because the search runs on the main thread.
// It handles the commands that can't wait for a search to end (stop, quit,
// isready) and queues everything else for the main thread.

#define QUEUE_CAPACITY 256

static char *queue[QUEUE_CAPACITY];
static int queue_head = 0;
static int queue_count = 0;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t queue_not_full = PTHREAD_COND_INITIALIZER;

// Searches are numbered in the order their `go` lines arrive. `stop` applies to
// every search received so far, including one still waiting in the queue, so a
// `stop` right behind a `go` can't be lost.
static atomic_uint go_received = 0;
static atomic_uint go_begun = 0;
static atomic_uint go_finished = 0;
static atomic_uint stop_up_to = 0;
static atomic_int reader_running = 0;

static void enqueue(const char *line) {
  char *copy = strdup(line);
  if (!copy)
    return;

  pthread_mutex_lock(&queue_lock);
  while (queue_count == QUEUE_CAPACITY)
    pthread_cond_wait(&queue_not_full, &queue_lock);
  queue[(queue_head + queue_count) % QUEUE_CAPACITY] = copy;
  queue_count++;
  pthread_cond_signal(&queue_not_empty);
  pthread_mutex_unlock(&queue_lock);
}

static int first_word_is(const char *line, const char *a, const char *b) {
  size_t n = strcspn(line, " \t");
  return (strlen(a) == n && strncmp(line, a, n) == 0) ||
         (b && strlen(b) == n && strncmp(line, b, n) == 0);
}

static void request_stop(void) {
  atomic_store(&stop_up_to, atomic_load(&go_received));
  atomic_store(&time_control.finished, 1);
}

static void *input_loop(void *arg) {
  (void)arg;

  static char buf[INPUT_BUF_SIZE];

  while (fgets(buf, sizeof(buf), stdin)) {

    buf[strcspn(buf, "\r\n")] = '\0';

    const char *line = buf + strspn(buf, " \t");
    const int search_active = atomic_load(&go_finished) != atomic_load(&go_received);

    if (first_word_is(line, "go", "g")) {
      atomic_fetch_add(&go_received, 1);
    }
    else if (first_word_is(line, "stop", NULL)) {
      if (search_active)
        request_stop();
      continue;
    }
    else if (first_word_is(line, "quit", "q")) {
      request_stop();
    }
    else if (search_active && first_word_is(line, "isready", NULL)) {
      printf("readyok\n");
      fflush(stdout);
      continue;
    }

    enqueue(line);
  }

  // stdin closed: nobody can ever send stop, so end any search and exit
  request_stop();
  enqueue("quit");
  return NULL;
}

void input_init(void) {
  pthread_t thread;
  atomic_store(&reader_running, 1);
  pthread_create(&thread, NULL, input_loop, NULL);
  pthread_detach(thread);
}

int input_is_running(void) {
  return atomic_load(&reader_running);
}

int input_get(char *buf, int size) {
  pthread_mutex_lock(&queue_lock);
  while (queue_count == 0)
    pthread_cond_wait(&queue_not_empty, &queue_lock);
  char *line = queue[queue_head];
  queue_head = (queue_head + 1) % QUEUE_CAPACITY;
  queue_count--;
  pthread_cond_signal(&queue_not_full);
  pthread_mutex_unlock(&queue_lock);

  snprintf(buf, (size_t)size, "%s", line);
  free(line);
  return 1;
}

void input_go_begin(void) {
  const unsigned id = atomic_fetch_add(&go_begun, 1) + 1;
  if (atomic_load(&stop_up_to) >= id)
    atomic_store(&time_control.finished, 1);
}

void input_go_end(void) {
  atomic_fetch_add(&go_finished, 1);
}
