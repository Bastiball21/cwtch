# Cwtch

Cwtch is a UCI chess engine written in C. It includes a self-play data generation tool and integrates with the `bullet` trainer to produce neural networks for evaluation. This document describes the full workflow: building, generating training data, training a network, and using it in the engine.

## Strength

Cwtch 7.1 against 6.5, one thread per engine, on the `UHO_Lichess_4852_v1` book:

| Test | Time control | Games | Elo |
|---|---|---|---|
| SPRT, passed | 8+0.08 | 192 | +229.2 ± 31.5 |
| SPRT, passed | 60+0.6 | 190 | +235.2 ± 25.8 |
| Fixed-length estimate | 8+0.08 | 1000 | +227.6 ± 15.6 |
| Fixed-length estimate | 60+0.6 | 400 | +226.3 ± 20.1 |

These compare two versions of the same engine, which usually overstates the gain against other engines. 7.1 hasn't been tested on a rating list yet. [CHANGELOG.md](CHANGELOG.md) has the settings, how the gain was built, and what changed in each version.

## Build

### Requirements

- Clang (or GCC) with support for C11
- Make
- zstd (for compressed data output)

### Compilation

```
make
```

This detects your host OS and CPU architecture and builds `./cwtch` for them. On `x86_64` one binary detects the CPU and dispatches NNUE inference and accumulator updates to the fastest supported instruction set at runtime (`avx512_vnni`, `avx512bw`, `avx2`, or `scalar`).

```
make test           # build, then run perft, SIMD agreement, bench determinism and UCI checks
```

For release builds:

```
make release        # Static Linux binary in ./releases/cwtch_7.1_linux_x86_64
make release-win    # Static Windows binary in ./releases/cwtch_7.1_windows_x86_64.exe
make release-all    # Linux, Windows, and aarch64 release binaries
```

`make clean` removes build output but leaves `releases/` alone.

## UCI Options

- `option name Hash type spin default 256 min 1 max 262144` - exact size in MB, not rounded to a power of two.
- `option name Threads type spin default 1 min 1 max 256`
- `option name Move Overhead type spin default 10 min 0 max 5000` - milliseconds kept back from the clock for GUI and network lag.
- `option name Clear Hash type button`
- `option name UCI_Chess960 type check default false`
- `option name LoadNet type string default`

`info` lines report `depth`, `seldepth`, `score cp` or `score mate`, `time`, `nodes`, `nps`, `hashfull` and `pv`. With no legal move the engine answers `bestmove 0000`. `go infinite` always waits for `stop`, and `stop` and `isready` are answered during a search.

## UCI Command Extensions

- `quit` / `q` - exit.
- `bench [depth]` - benchmark search to a given depth (default 10).
- `eval` / `e` - print the net's evaluation of the current position, from the side to move.
- `board` / `b` - print board.
- `perft` / `f depth` - run perft.
- `pt [max_depth]` - run perft suite.
- `et` - evaluation test.
- `net` / `n` - display network architecture.
- `loadnet` / `ln path` - load external quantized network.
- `datagen` / `dg dir positions [book_file|frc|dfrc]` - generate self-play training data.

### Datagen Command Details

```
datagen <directory> <positions> [book_file|frc|dfrc]
```

- `directory` - output folder for `.vf.zst` files.
- `positions` - target number of training positions (samples) to generate.
- Optional third argument:
  - `book_file` - path to a text file containing starting positions.
    - EPD / FEN: one FEN per line (EPD operations ignored; first six fields used).
    - PGN: only used if the game has a `[FEN "..."]` header. Games without FEN are skipped.
  - `frc` - generate games from random Fischer Random Chess (Chess960) starting positions.
  - `dfrc` - generate games from random Double Fischer Random Chess starting positions.
- If no third argument is given, datagen uses random plies from the standard starting position.

All game parameters can be adjusted using the constants in `src/datagen.c`. Key constants:

- `DG_SEARCH_NODES` (default 5000) - soft nodes per move: the iteration in progress is finished, so scores come from complete searches.
- `DG_HARD_NODES` (default 100000) - hard limit per move.
- `DG_DOWNSAMPLE_RATE` (default 1) - record 1 in N positions.
- `DG_MIN_RECORD_PLY_STD` / `DG_MIN_RECORD_PLY_FRC` (default 10 / 8) - skip the first N plies after the opening.
- `DG_EXTRA_PLIES_BOOK` / `_FRC` / `_DFRC` (default 3 / 8 / 4) - most random extra plies after the start.
- `BLOOM_SIZE_BYTES` (default 64 MB) - Bloom filter size for deduplication.
- `BLOOM_RESET_INSERTS` (default 50M) - reset the Bloom filter every N inserts.

If a write fails (a full disk, for example) datagen stops, records `write_failed 1` in the manifest and exits with an error.

Examples:

```
./cwtch datagen data 1000000                # random openings
./cwtch datagen data 1000000 book.txt       # EPD/FEN book
./cwtch datagen data 1000000 games.pgn      # PGN with FEN tags
./cwtch datagen data 1000000 frc            # Chess960 random starts
./cwtch datagen data 1000000 dfrc           # Double FRC random starts
```

### Parallel Datagen

Use the included `run_datagen.sh` script to launch multiple datagen processes in parallel and track progress with ETA.

```
./run_datagen.sh frc
./run_datagen.sh /path/to/book.epd
```

The script writes compressed data to `$OUTPUT_BASE/data` and logs to `$OUTPUT_BASE/logs`. `BINARY`, `OUTPUT_BASE`, `TARGET_POSITIONS` and `THREADS` can all be set in the environment.

## Data Format (`.vf.zst`)

Datagen writes compact game records, compressed with zstd. Each game is stored as:

- Bytes 0-31: starting board (packed, same layout as before)
- Byte 32: WDL result (0 = black win, 1 = draw, 2 = white win)
- Bytes 33-34: number of recorded positions (little-endian u16)
- Then N x 4-byte records (2-byte move + 2-byte score from White's perspective)

This format stores only the starting board and the moves. The trainer replays moves to reconstruct each intermediate position, which reduces file size from ~35 bytes/position to ~4-5 bytes/position while preserving the exact same training data.

### Compression

Files are compressed with zstd. The compression level is set in `datagen.c` (default 1). Lower levels (1-5) are faster and produce slightly larger files; higher levels (19-22) are slower but smaller. The trainer reads any level transparently.

### Deduplication and Filtering

Datagen filters as it plays:

- Bloom filter: skips positions whose Zobrist hash was already seen, within one process. It resets every 50 million inserts to avoid saturation.
- Skip early plies, positions in check, and positions with only one legal move.

Filtered positions keep their move, so the trainer can replay the game, but carry a sentinel score and are never trained on.

The trainer filters again, so the rule can change without regenerating data: it drops positions in check and scores above `SCORE_FILTER`. `TACTICAL_FILTER=1` also drops positions whose best move is a capture, en passant or promotion, but read [Choosing a Recipe](#choosing-a-recipe) first.

## Training

The trainer is a separate Rust project (`trainer/`). It reads `.vf.zst` self-play files and Stockfish `.binpack` files directly.

### Trainer Setup

1. Install Rust, and for GPU training the CUDA toolchain bullet needs.

2. Check out its two dependencies beside this repository, at the revisions the patches apply to:

```
cd ..
git clone https://github.com/jw1912/bullet
git -C bullet checkout cebc78a093d92cbc87e56cfef049184c225270b0
git -C bullet apply ../cwtch/trainer/patches/bullet.patch
git clone https://github.com/Disservin/binpack-rust sfbinpack-patched
git -C sfbinpack-patched checkout 5b82b204fecf8d5eca3af813511c63259a125a99
git -C sfbinpack-patched apply ../cwtch/trainer/patches/sfbinpack.patch
```

`bullet.patch` pins the GPU architecture string for current NVIDIA cards. `sfbinpack.patch` guards a pawn attack lookup against an out of range square.

3. Build with `cargo build --release` in `trainer/`.

4. Configure with environment variables:

- `DATA_PATH` (default `/mnt/d/Bulk-Data`) - directory containing `.vf.zst` / `.binpack` files.
- `VF_FRACTION` (default 0.5) - share of every shuffle buffer taken from self-play; the rest comes from binpacks. Set to 1.0 for self-play only.
- `SF_SCORE_MULT` (default 0.87) - multiplier on binpack scores, to match the self-play score scale.
- `BUFFER_MB` (default 4096) - shuffle buffer size. Two buffers exist at once.
- `SCORE_FILTER` (default 1500) - drop positions with a larger absolute score.
- `TACTICAL_FILTER` (default 0) - set to 1 to drop positions whose best move is tactical.
- `HIDDEN_SIZE` (default 1536) - hidden layer size, must match `NET_H1_SIZE` in the engine.
- `BATCH_SIZE` (default 16384), `BATCHES_PER_SUPERBATCH` (default 6104), `SUPERBATCH` (default 600), `SAVE_RATE` (default 50).
- `LEARNING_RATE` (default 0.001), `WARMUP_BATCHES` (default 200), `WDL_VALUE` (default 0.4).
- `OUTPUT_DIR` (default `nets`), `NET_ID` (default `cwtch`).
- `VF_WORKERS` (default 6), `VF_STREAMS_PER_WORKER` (default 12), `SF_WORKERS` (default 6), `TRAIN_THREADS` (default 24).
- `LOAD_CHECKPOINT` (none), `SAVE_CHECKPOINT` (none), `START_SUPERBATCH` (default 1).

5. Run the trainer. `run_trainer.sh` resumes from the newest checkpoint after a crash:

```
./run_trainer.sh
```

### Loader

Training quality depends heavily on what's in each batch, so the loader streams rather than reading files whole:

- Self-play is read one game at a time from many open files, and binpacks one chunk at a time from all of them.
- Each shuffle buffer holds exactly the requested mix of sources.
- Files are dealt in shuffled passes, and binpack streams start at a random chunk, so runs that never finish a file still sample all of it.
- The next buffer is filled and shuffled in the background while the current one trains.

### Checking Score Scales

`DATA_STATS=<positions> ./target/release/trainer` fits, per source, the scale at which `sigmoid(score / scale)` best predicts the game result, and prints how many positions the filter keeps. Run this before mixing a new data source: on the current data self-play fits about 285 and binpacks about 327, which is where the default `SF_SCORE_MULT` of 0.87 comes from.

### Model Architecture

- Input: 768 features per king bucket, 16 buckets per perspective (mirrored horizontally), total 12288 inputs per side, trained with a shared 768-input factoriser merged at save time.
- Hidden layer: 1536 neurons (compile-time configurable via `NET_H1_SIZE`), Clipped Squared ReLU (`screlu`) activation.
- Output: 8 material-count buckets, linear output (`int16` weights clipped to `[-127, 127]` for overflow-free SIMD `madd` / `dpwssd`).
- Quantization: QA=255, QB=64, scale=400.

The saved network is written to the `nets/` directory in a format compatible with the engine (`quantised.bin`). `bullet_lib` stores `l0w` in column-major layout (`[12288][1536]`), which directly matches the engine's input-major accumulator layout without transposition.

### Choosing a Recipe

These were measured with the current search, at equal nodes, between nets that each trained for 100 superbatches on this repository's data:

| Change | Elo |
|---|---|
| `TACTICAL_FILTER=1` with `SCORE_FILTER=10000`, against unfiltered with 1500 | about -200 |
| `VF_FRACTION=0.5` against `VF_FRACTION=0.1` | +42 +/- 20 |
| Current loader at 50/50 against the old file-at-a-time loader | +24 +/- 21 |

A tactically filtered net never sees a position with a capture available, so it misjudges them badly: after a queen is taken but can be recaptured, a filtered net reads about -1600 where an unfiltered one reads about -600. This search prunes on static eval at nodes where captures are still available, so unfiltered nets play far better with it. 6.5's net was filtered, and scores 235 Elo below `cwtch-600` on this search. The filter's effect depends on the pruning, so retest it if pruning changes.

At 8+0.08 on this search, a 512 net scored about 23 Elo below a 1536 net with the same training. The old 4096 `cwtch-900` net scored -102 +/- 26 against 6.5, about 290 below `cwtch-600`, but it came from the old loader with half the batch size, so that isn't a fair test of the size.

### Fine-Tuning

Continuing a finished net at a low learning rate is the cheapest gain available: 50 superbatches, about an hour, took `cwtch-600` to +39 +/- 19 Elo at equal nodes and +51 +/- 18 at 8+0.08. This is the net in the repository. The same fine-tune with `TACTICAL_FILTER=1` scored -60 +/- 25 at equal nodes.

```
cd trainer
LOAD_CHECKPOINT=nets/cwtch-600 OUTPUT_DIR=nets_ft NET_ID=ft SUPERBATCH=50 LEARNING_RATE=0.0001 ./target/release/trainer
```

### Evaluating a Net

Loss doesn't rank nets. Compare them in games, at fixed nodes so that speed doesn't blur the result. Checkpoints from part way through a run are not comparable with finished nets, because the learning rate is still high when they are saved.

To use a net, copy its `quantised.bin` over the one in the repository root and rebuild, or use the `loadnet` command.

## Chess960 / FRC / DFRC Support

The engine supports Fischer Random Chess (Chess960) via the `UCI_Chess960` option. Datagen can generate:

- FRC - standard symmetric Chess960 positions (`frc` argument).
- DFRC - Double Fischer Random Chess, where White and Black back ranks are independently randomized (`dfrc` argument).

Castling rights are stored using Shredder-FEN (file letters), and castling moves are generated based on the actual rook positions. DFRC offers 960 x 960 = 921,600 possible starting positions.

## Book Formats

- EPD / FEN: plain text, one position per line. Extra EPD operations are ignored.
- PGN: only `[FEN "..."]` headers are used. If a game has no FEN tag, it is skipped. For full PGN support (e.g., extracting FENs from move text), pre-process with a tool like `pgn-extract`.

`MAX_BOOK_FENS` in `datagen.c` limits books to 3,000,000 lines. Invalid lines are skipped with a warning.

## Testing

`bin/sprt [stc|ltc]` plays `./cwtch_test` against `./cwtch_base` with SPRT bounds [0, 10] in normalized Elo. It deletes any `sprt.pgn` in the directory it runs from. `MARGIN` sets fastchess's time margin in milliseconds (default 50). Keep it small, because a large margin hides engines overrunning their clock.

## Future Improvements

- Search: parameter tuning with SPSA, tablebases, threat-aware history, upcoming-repetition detection.
- Lazy SMP tuning: a thread pool, and per-thread histories.
- Nets: a longer fine-tune, and pruning retuned for tactically filtered nets.

## Acknowledgements

- Chess Programming Wiki: https://www.chessprogramming.org/Main_Page
- CCRL: https://computerchess.org.uk/ccrl/4040
- UCI Protocol: https://backscattering.de/chess/uci
- TalkChess: https://talkchess.com
- bullet trainer: https://github.com/jw1912/bullet
- Engine Programming Discord: https://discord.gg/pntchvGU
- Magic Bitboards: https://analog-hors.github.io/site/magic-bitboards
- incbin: https://github.com/graphitemaster/incbin
- viriformat: https://github.com/cosmobobak/viriformat