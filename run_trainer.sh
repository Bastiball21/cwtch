#!/bin/bash
set -u

cd ~/cwtch/trainer

DATA_PATH="${DATA_PATH:-/mnt/d/Bulk-Data}"
HIDDEN_SIZE="${HIDDEN_SIZE:-1536}"
BATCH_SIZE="${BATCH_SIZE:-16384}"
BATCHES_PER_SUPERBATCH="${BATCHES_PER_SUPERBATCH:-6104}"
SUPERBATCH="${SUPERBATCH:-600}"
SAVE_RATE="${SAVE_RATE:-50}"
TRAIN_THREADS="${TRAIN_THREADS:-24}"

NETS=~/cwtch/trainer/nets
LATEST=$(ls -d "$NETS"/cwtch-* 2>/dev/null | sed 's|.*/cwtch-||' | grep -E '^[0-9]+$' | sort -n | tail -1)
LATEST="${LATEST:-0}"

while [ "$LATEST" -lt "$SUPERBATCH" ]; do
    NEXT=$((LATEST + 1))
    LOG=~/cwtch_train_h${HIDDEN_SIZE}_sb${NEXT}_$(date +%s).log
    echo "=== Training (H=$HIDDEN_SIZE) superbatch $NEXT -> $SUPERBATCH (log: $LOG) ==="

    if [ "$LATEST" -gt 0 ] && [ -d "$NETS/cwtch-$LATEST" ]; then
        LOAD_ARG="$NETS/cwtch-$LATEST"
    else
        LOAD_ARG=""
    fi

    if [ -n "$LOAD_ARG" ]; then
        DATA_PATH="$DATA_PATH" \
        HIDDEN_SIZE="$HIDDEN_SIZE" \
        LOAD_CHECKPOINT="$LOAD_ARG" \
        START_SUPERBATCH="$NEXT" \
        SUPERBATCH="$SUPERBATCH" \
        BATCH_SIZE="$BATCH_SIZE" \
        BATCHES_PER_SUPERBATCH="$BATCHES_PER_SUPERBATCH" \
        TRAIN_THREADS="$TRAIN_THREADS" \
        SAVE_RATE="$SAVE_RATE" \
        ./target/release/trainer 2>&1 | tee -a "$LOG"
    else
        DATA_PATH="$DATA_PATH" \
        HIDDEN_SIZE="$HIDDEN_SIZE" \
        START_SUPERBATCH=1 \
        SUPERBATCH="$SUPERBATCH" \
        BATCH_SIZE="$BATCH_SIZE" \
        BATCHES_PER_SUPERBATCH="$BATCHES_PER_SUPERBATCH" \
        TRAIN_THREADS="$TRAIN_THREADS" \
        SAVE_RATE="$SAVE_RATE" \
        ./target/release/trainer 2>&1 | tee -a "$LOG"
    fi

    NEW=$(ls -d "$NETS"/cwtch-* 2>/dev/null | sed 's|.*/cwtch-||' | grep -E '^[0-9]+$' | sort -n | tail -1)
    NEW="${NEW:-0}"
    if [ "$NEW" -le "$LATEST" ]; then
        echo "=== No progress, sleeping 5m ==="
        sleep 300
    else
        LATEST="$NEW"
        sleep 10
    fi
done
echo "=== DONE at cwtch-$LATEST ==="
