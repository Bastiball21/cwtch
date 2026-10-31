# Cwtch

Cwtch is a UCI chess engine written in C. It includes a self-play data generation tool and integrates with the `bullet` trainer to produce neural networks for evaluation. This document describes the full workflow: building, generating training data, training a network, and using it in the engine.

## Build

### Requirements

- Clang (or GCC) with support for C11
- Make
- zstd (for compressed data output)

### Compilation

```
make
```

This automatically detects your host OS (`linux`, `windows`, `macos`) and CPU architecture (`x86_64`, `aarch64`) and builds a universal `./cwtch` binary. On `x86_64`, the binary automatically detects the user's CPU model and dispatches NNUE inference and accumulator updates to the fastest supported instruction set at runtime (`avx512_vnni`, `avx512bw`, `avx2`, or `scalar`).

For release builds:

```
make release        # Universal Linux binary in ./releases/cwtch_7.0_linux_x86_64
make release-win    # Universal Windows binary in ./releases/cwtch_7.0_windows_x86_64.exe
make release-all    # Linux, Windows, and aarch64 release binaries
```

## UCI Options

- `option name Hash type spin default 256 min 1 max 262144`
- `option name Threads type spin default 1 min 1 max 256`
- `option name UCI_Chess960 type check default false`
- `option name LoadNet type string default`

## UCI Command Extensions

- `quit` / `q` - exit.
- `bench [depth]` - benchmark search to a given depth (default 10).
- `eval` / `e` - print current position evaluation.
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

- `DG_SEARCH_NODES` (default 3000) - nodes per move during self-play.
- `DG_DOWNSAMPLE_RATE` (default 3) - record 1 in N positions.
- `DG_MIN_RECORD_PLY` (default 16) - skip the first N plies (openings).
- `DG_EXTRA_PLIES_MAX` (default 6) - random extra plies after book/FRC/DFRC start.
- `BLOOM_SIZE_BYTES` (default 64 MB) - Bloom filter size for deduplication.
- `BLOOM_RESET_POSITIONS` (default 50M) - reset Bloom filter every N positions.

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

The script writes compressed data to `/mnt/d/cwtch_datagen/data` and logs to `/mnt/d/cwtch_datagen/logs`. Edit the constants at the top of the script to change `TARGET_POSITIONS` and `THREADS`.

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

- Bloom filter: skips positions whose Zobrist hash was already seen. Resets every 50 million positions to avoid saturation.
- Downsampling: only records 1 in N positions to reduce temporal correlation.
- Skip early plies: ignores the first 16 plies to avoid over-representing standard openings.

## Training

The trainer is a separate Rust project (`trainer/`). It uses a custom `VfLoader` to read compact game records and decompress `.vf.zst` files directly.

### Trainer Setup

1. Ensure you have Rust installed.

2. Add dependencies to `Cargo.toml`:

```
[dependencies]
bullet_lib = { git = "https://github.com/jw1912/bullet", features = ["cuda"] }
bullet_trainer = { git = "https://github.com/jw1912/bullet" }
zstd = "0.13"
rand = "0.8"
```

3. Configure via environment variables (or edit defaults in `main.rs`):

- `DATA_PATH` (default `/mnt/d/Bulk-Data`, or `D:\Bulk-Data`) - directory containing `.vf.zst` / `.binpack` files.
- `HIDDEN_SIZE` (default 1536) - hidden layer size.
- `OUTPUT_BUCKETS` (default 8) - number of output buckets.
- `BATCH_SIZE` (default 16384) - training batch size.
- `SUPERBATCH` (default 600) - number of superbatches.
- `LEARNING_RATE` (default 0.001) - initial learning rate.
- `WARMUP_BATCHES` (default 200) - LR warmup batches.
- `LOAD_CHECKPOINT` (none) - path to checkpoint to resume from.
- `SAVE_CHECKPOINT` (none) - path to save checkpoint after training.

4. Run the trainer:

```
./run_trainer.sh
```

### Model Architecture

- Input: 768 features per king bucket, 16 buckets per perspective (mirrored horizontally), total 12288 inputs per side, trained with a shared 768-input factoriser merged at save time.
- Hidden layer: 1536 neurons (compile-time configurable via `NET_H1_SIZE`), Clipped Squared ReLU (`screlu`) activation.
- Output: 8 material-count buckets, linear output (`int16` weights clipped to `[-127, 127]` for overflow-free SIMD `madd` / `dpwssd`).
- Quantization: QA=255, QB=64, scale=400.

The saved network is written to the `nets/` directory in a format compatible with the engine (`quantised.bin`). `bullet_lib` stores `l0w` in column-major layout (`[12288][1536]`), which directly matches the engine's input-major accumulator layout without transposition.

### Two-Stage Training Pipeline

For best results, train in stages:

1. Bulk pre-training - 10 billion positions at 3k nodes (broad knowledge).
2. Fine-tuning - 100-200 million positions at 20k nodes (accurate eval).
3. Final polish - 10-30 million positions at 100k nodes (sharp tactics and endgames).

Use checkpoints to preserve weights and optimizer state between stages:

```
# Stage 1: bulk
DATA_PATH=bulk_data LEARNING_RATE=0.001 SUPERBATCH=600 SAVE_CHECKPOINT=bulk.bin cargo run --release

# Stage 2: fine-tune
DATA_PATH=finetune_data LOAD_CHECKPOINT=bulk.bin LEARNING_RATE=0.0001 SUPERBATCH=50 SAVE_CHECKPOINT=finetuned.bin cargo run --release

# Stage 3: polish (optional)
DATA_PATH=polish_data LOAD_CHECKPOINT=finetuned.bin LEARNING_RATE=0.00005 SUPERBATCH=20 SAVE_CHECKPOINT=final.bin cargo run --release
```

### Data Generation and Training Pipeline

1. Generate data with the engine:
   ```
   ./cwtch datagen /path/to/data 1000000 book.txt
   ```
2. Ensure the output directory contains `.vf.zst` files.
3. Run the trainer as shown above.
4. The trainer saves `quantised.bin` into `trainer/nets/`.
5. Copy `quantised.bin` to the engine's directory and use the `loadnet` command, or embed it via `incbin` and rebuild.

## Chess960 / FRC / DFRC Support

The engine supports Fischer Random Chess (Chess960) via the `UCI_Chess960` option. Datagen can generate:

- FRC - standard symmetric Chess960 positions (`frc` argument).
- DFRC - Double Fischer Random Chess, where White and Black back ranks are independently randomized (`dfrc` argument).

Castling rights are stored using Shredder-FEN (file letters), and castling moves are generated based on the actual rook positions. DFRC offers 960 x 960 = 921,600 possible starting positions.

## Book Formats

- EPD / FEN: plain text, one position per line. Extra EPD operations are ignored.
- PGN: only `[FEN "..."]` headers are used. If a game has no FEN tag, it is skipped. For full PGN support (e.g., extracting FENs from move text), pre-process with a tool like `pgn-extract`.

Increase `MAX_BOOK_FENS` in `datagen.c` if your book has more than 1,000,000 lines (e.g., UHO_Lichess_4852_v1.epd has 2.6M).

## Recommended Data Mix

For a strong 4096 net, a balanced dataset is:

- DFRC: 2.5 billion positions (most opening diversity, 921,600 starts).
- FRC: 2.5 billion positions (symmetric Chess960, 960 starts with extra random plies).
- Standard (book): 5 billion positions (use a large book such as UHO with 2.6M lines).
- Total: 10 billion positions.

Then add 100-200 million at 20k nodes for fine-tuning, and optionally 10-30 million at 100k for final polish.

## Future Improvements

- Universal binaries with runtime CPU dispatch for AVX2 / AVX-512.
- Search improvements: Lazy SMP tuning, tablebases, better move ordering.
- Further network scaling: 5120 or 6144 hidden if data allows.

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