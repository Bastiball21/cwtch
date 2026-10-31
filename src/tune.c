#include "tune.h"

#ifdef TUNE

#include <stdio.h>
#include <string.h>
#include "search.h"

#define X(name, def, lo, hi, step) int name = def;
TUNABLES(X)
#undef X

void tune_print_options(void) {
#define X(name, def, lo, hi, step) printf("option name " #name " type spin default %d min %d max %d\n", def, lo, hi);
  TUNABLES(X)
#undef X
}

int tune_set_option(const char *option, int value) {
#define X(name, def, lo, hi, step) \
  if (strcmp(option, #name) == 0) { \
    name = value; \
    init_lmr(); \
    return 1; \
  }
  TUNABLES(X)
#undef X
  return 0;
}

#else

// ISO C forbids an empty translation unit
typedef int tune_unused;

#endif
