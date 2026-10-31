#ifndef HH_H
#define HH_H

#include <stdint.h>

#define MAX_HH 1024

void hh_reset(void);
void hh_push(uint64_t hash);
void hh_set_root(void);
void hh_init_thread(void);
void hh_store(int search_ply, uint64_t hash);
int is_draw(int search_ply, uint64_t hash, int hmc);

#endif
