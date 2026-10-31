#ifndef TUNE_H
#define TUNE_H

// Search parameters. Normally these are compile time constants. Built with -DTUNE
// each becomes a UCI spin option, for tools/spsa.py.
//
//   X(name, default, min, max, spsa step)
#define TUNABLES(X) \
  X(lmr_base, 73, 20, 130, 8) \
  X(lmr_div, 236, 150, 350, 15) \
  X(lmr_hist_div, 12000, 5000, 30000, 1500) \
  X(lmr_deeper, 60, 20, 120, 8) \
  X(rfp_margin, 75, 40, 130, 7) \
  X(razor_base, 300, 100, 600, 40) \
  X(razor_mul, 100, 30, 250, 20) \
  X(nmp_static_margin, 20, -50, 120, 10) \
  X(nmp_eval_div, 200, 80, 400, 25) \
  X(nmp_depth_div, 3, 2, 6, 1) \
  X(pc_margin, 200, 100, 350, 20) \
  X(pc_improving, 50, 0, 120, 10) \
  X(lmp_base, 3, 1, 8, 1) \
  X(fut_base, 100, 30, 220, 15) \
  X(fut_mul, 100, 40, 200, 12) \
  X(hist_prune, 4000, 1000, 9000, 500) \
  X(see_quiet, 30, 5, 80, 5) \
  X(cap_fut_base, 150, 40, 320, 20) \
  X(cap_fut_mul, 100, 40, 220, 12) \
  X(see_noisy, 90, 30, 180, 10) \
  X(se_margin, 4, 1, 10, 1) \
  X(dext_margin, 16, 2, 50, 4) \
  X(bonus_mul, 18, 6, 40, 2) \
  X(bonus_diff_div, 4, 1, 16, 1) \
  X(bonus_max, 2020, 800, 5000, 200) \
  X(asp_delta, 12, 5, 30, 2) \
  X(qs_delta, 200, 50, 400, 25) \
  X(corr_pawn, 110, 20, 256, 15) \
  X(corr_minor, 70, 0, 200, 12) \
  X(corr_major, 70, 0, 200, 12) \
  X(corr_nonpawn, 70, 0, 200, 12) \
  X(corr_cont, 60, 0, 200, 12) \
  X(tm_base, 54, 25, 90, 4) \
  X(tm_inc, 85, 40, 120, 6) \
  X(tm_soft, 76, 40, 120, 6) \
  X(tm_hard, 304, 150, 500, 25)

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
