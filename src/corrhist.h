#ifndef CORRHIST_H
#define CORRHIST_H

#include <string.h>
#include <stdint.h>
#include "types.h"
#include "pos.h"
#include "nodes.h"

#define CORR_SIZE 16384  // power of two
#define CORR_GRAIN 256   // entry units per cp
#define CORR_LIMIT (32 * CORR_GRAIN)
#define CORR_WEIGHT_MAX 16
#define CORR_WEIGHT_SCALE 256

void clear_corrhist(void);
int correct_eval(const Node *node, const int ev);
void update_corrhist(const Node *node, const int depth, const int diff);

#endif
