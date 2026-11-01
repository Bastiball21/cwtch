#ifndef TUNE_H
#define TUNE_H

// Search parameters. Normally these are compile time constants. Built with -DTUNE
// each becomes a UCI spin option, for tools/spsa.py.
//
//   X(name, default, min, max, spsa step)
#define TUNABLES(X) \
  X(lmr_base, 40, 20, 130, 8) \
  X(lmr_div, 346, 150, 350, 15) \
  X(lmr_hist_div, 22106, 5000, 30000, 1500) \
  X(lmr_deeper, 54, 20, 120, 8) \
  X(rfp_margin, 103, 40, 130, 7) \
  X(razor_base, 261, 100, 600, 40) \
  X(razor_mul, 97, 30, 250, 20) \
  X(nmp_static_margin, -14, -50, 120, 10) \
  X(nmp_eval_div, 387, 80, 400, 25) \
  X(nmp_depth_div, 3, 2, 6, 1) \
  X(pc_margin, 300, 100, 350, 20) \
  X(pc_improving, 59, 0, 120, 10) \
  X(lmp_base, 5, 1, 8, 1) \
  X(fut_base, 173, 30, 220, 15) \
  X(fut_mul, 87, 40, 200, 12) \
  X(hist_prune, 3770, 1000, 9000, 500) \
  X(see_quiet, 11, 5, 80, 5) \
  X(cap_fut_base, 206, 40, 320, 20) \
  X(cap_fut_mul, 43, 40, 220, 12) \
  X(see_noisy, 99, 30, 180, 10) \
  X(se_margin, 1, 1, 10, 1) \
  X(dext_margin, 33, 2, 50, 4) \
  X(bonus_mul, 36, 6, 40, 2) \
  X(bonus_diff_div, 8, 1, 16, 1) \
  X(bonus_max, 3773, 800, 5000, 200) \
  X(asp_delta, 9, 5, 30, 2) \
  X(qs_delta, 333, 50, 400, 25) \
  X(corr_pawn, 170, 20, 256, 15) \
  X(corr_major, 161, 0, 200, 12) \
  X(corr_nonpawn, 178, 0, 200, 12) \
  X(corr_cont, 146, 0, 200, 12) \
  X(tm_base, 46, 25, 90, 4) \
  X(tm_inc, 77, 40, 120, 6) \
  X(tm_soft, 56, 40, 120, 6) \
  X(tm_hard, 403, 150, 500, 25)

#ifdef TUNE

#define X(name, def, lo, hi, step) extern int name;
TUNABLES(X)
#undef X

void tune_print_options(void);
int tune_set_option(const char *name, int value);

#else

#define X(name, def, lo, hi, step) enum { name = def };
TUNABLES(X)
#undef X

#endif

#endif
