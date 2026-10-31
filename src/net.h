#ifndef NET_H
#define NET_H

#include "nodes.h"

int init_weights(void);
int load_weights_from_file(const char *path);
int net_eval(Node *node);
void net_slow_rebuild_accs(Node *node);
void update_accs(Node *node, const int16_t (*src)[NET_H1_SIZE]);
void lazy_update_accs(Node *node);
void net_init_thread(void);
const char *net_simd_name(void);
const char *net_os_name(void);
const char *net_arch_name(void);
const char *net_cpu_brand(void);
int net_set_simd(const char *name);

static inline int net_base(const int piece, const int sq) {
  return (((piece << 6) | sq) * NET_H1_SIZE);
}

#endif
