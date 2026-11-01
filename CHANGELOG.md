# Changelog

Elo figures for 7.1 come from matches whose PGNs, logs and settings are attached to the 7.1 release. Figures for earlier versions are what was recorded when each version was made, and they have not been retested.

## 7.1 (2026-11-01)

### Strength against 6.5

| Test | Time control | Hash | Games | W-L-D | Elo |
|---|---|---|---|---|---|
| SPRT, passed | 8+0.08 | 16 MB | 192 | 120-9-63 | +229.2 ± 31.5 |
| SPRT, passed | 60+0.6 | 256 MB | 190 | 113-1-76 | +235.2 ± 25.8 |
| Fixed-length estimate | 8+0.08 | 16 MB | 1000 | 624-49-327 | +227.6 ± 15.6 |
| Fixed-length estimate | 60+0.6 | 256 MB | 400 | 239-10-151 | +226.3 ± 20.1 |

All four were played on one thread per engine with the `UHO_Lichess_4852_v1` book, with no time losses, illegal moves or crashes. Error bars are 95%. SPRTs use normalized Elo bounds [0, 10], alpha 0.05 and beta 0.10, and stop as soon as they're decided, so their Elo figures are noisier than the fixed-length ones.

These are matches against an older version of the same engine on an unbalanced opening book. Gains measured this way usually overstate the gain against other engines. 7.1 hasn't yet been tested on a rating list or against other engines.

### How the gain was built

Each row was tested against the build before it, at 8+0.08 unless noted. These don't add up to the total: gains overlap, and tests between versions of the same engine tend to run high. The direct matches above are the numbers to trust.

| Change | Result | Games | Elo |
|---|---|---|---|
| UCI, time management, transposition table | SPRT pass | 314 | +83.5 ± 21.7 |
| Qsearch evasions and SEE, staged move ordering | SPRT pass | 652 | +36.4 ± 15.4 |
| Histories, pruning, LMR, null move | SPRT pass | 236 | +141.3 ± 25.2 |
| Correction tables | Neutral, kept for tuning | 2000 | +1.9 ± 8.2 |
| History refinements | SPRT pass | 1182 | +19.1 ± 10.8 |
| Threat-aware history | Neutral, not included | 3000 | +2.4 ± 6.8 |
| SPSA tuning, 24,000 games at 3+0.03 | SPRT pass | 456 | +46.0 ± 17.9 |
| SPSA tuning, at 60+0.6 | SPRT pass | 578 | +33.8 ± 13.7 |
| Fine-tuned net | Fixed-length estimate | 500 | +51.1 ± 17.5 |

The 8+0.08 steps used a 250 ms time margin. The direct matches against 6.5 and the 60+0.6 tuning test used 50 ms. A separate 300 games at 1+0.01 with no margin at all had no time losses.

### Search

- Lock-free transposition table: four 16 byte entries per cache line, replacement by depth and age, stored static eval, and exact `Hash` sizes rather than rounding down to a power of two
- Qsearch searches evasions when in check, so it can see mate, and prunes with SEE and delta pruning
- Staged move ordering: good captures, then quiets, then losing captures and underpromotions
- Main, piece-to and continuation history 1, 2 and 4 plies back, plus a second killer
- ProbCut, late move pruning, futility pruning, history pruning and SEE pruning
- Null move scaled by depth and margin, with verification at high depth
- LMR adjusted for PV, improving, cut nodes, checks, noisy TT moves, history and sibling cutoffs, and reduced searches can re-search deeper or shallower
- Correction history for pawn, major piece, non-pawn and previous-move keys, and evals fade near the 50 move limit
- Search parameters tuned with SPSA
- `MAX_PLY` raised to 128
- Fixes: LMR could search a move deeper than normal, an excluded search with no other legal move returned a mate or stalemate score, and SEE undervalued the king

### Net

- Fine-tuned from the 1536 `cwtch-600` net: 50 superbatches at a low learning rate. Architecture is unchanged at (768×16 mirrored → 1536)×2 → 8 outputs.

### UCI and time management

- New options: `Move Overhead` and `Clear Hash`
- Soft and hard time limits, scaled by best-move stability, score trend and how many nodes the best move took. Limits are always capped by the time on the clock.
- `info` reports `score mate`, `seldepth` and `hashfull`
- Fixes:
    - a `stop` right behind `go` could hang the engine
    - the engine used a full core while idle
    - `isready` went unanswered during a search
    - malformed `setoption`, `position` and `go` lines could crash it
    - `go infinite` could answer before `stop`
    - no legal move now gives `bestmove 0000`

### Datagen and trainer

- Datagen uses a soft node limit, so scores come from finished iterations. It stops and reports failure when a write fails, and skips invalid book lines.
- Trainer loader streams games and chunks from many files, with an exact mix per shuffle buffer (`VF_FRACTION`)
- `DATA_STATS` fits the score scale per data source
- The tactical filter is switchable and off by default, because it cost about 200 Elo at equal nodes with this search. README.md has the measurements.

### Build and testing

- `make` targets the host rather than always x86-64
- Linux releases are static, and `make clean` leaves `releases/` alone
- `make test` and CI run perft, SIMD agreement, bench determinism and UCI checks
- `tools/spsa.py` tunes the parameters in `src/tune.h`

## 7.0

Not released on its own.

- 1536 neuron net with 16 king buckets and clipped squared ReLU
- One binary for x86-64 with runtime SIMD dispatch: AVX-512 VNNI, AVX-512 BW, AVX2 or scalar
- Compact `.vf.zst` datagen with FRC and DFRC support, deduplication and clean shutdown
- Trainer added to the repository

On the old search, this build's net scored about 37 Elo below 6.5 at 8+0.08 (466 games), and about 38 below at equal nodes (200 games). The search work in 7.1 is what turned that around.

## 6.5

Never committed separately; its changes are in the snapshot commit `693fff3`. It is the baseline for the 7.1 results, using a 512 neuron net with 4 king buckets.

## 6.4

- Chess960 and FRC support, including Shredder-FEN parsing
- Atomic spinlock for thread safety
- Windows thread limit fixed, AVX2 enabled
- Maximum `Hash` raised for TCEC

## 6.3

- Lockless Lazy SMP with local node batching and thread-local NNUE Finny caches

## 6.2

- StatScore history, razoring, and null move and LMR scaling (recorded at the time as +7.89 Elo)

## 6.1

- Countermove history (recorded at the time as +15 Elo at STC)
