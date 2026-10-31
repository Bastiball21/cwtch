#!/bin/bash
# Trains until SUPERBATCH, resuming from the newest checkpoint after any crash.
# Every setting main.rs reads can be set in the environment.
set -u

cd "$(dirname "$0")/trainer" || exit 1

export DATA_PATH="${DATA_PATH:-/mnt/d/Bulk-Data}"
export HIDDEN_SIZE="${HIDDEN_SIZE:-1536}"
export BATCH_SIZE="${BATCH_SIZE:-16384}"
export BATCHES_PER_SUPERBATCH="${BATCHES_PER_SUPERBATCH:-6104}"
export SUPERBATCH="${SUPERBATCH:-600}"
export SAVE_RATE="${SAVE_RATE:-50}"
export TRAIN_THREADS="${TRAIN_THREADS:-24}"
export OUTPUT_DIR="${OUTPUT_DIR:-nets}"
export NET_ID="${NET_ID:-cwtch}"

mkdir -p "$OUTPUT_DIR"

latest_saved() {
  ls -d "$OUTPUT_DIR/$NET_ID"-* 2>/dev/null | sed "s|.*/$NET_ID-||" | grep -E '^[0-9]+$' | sort -n | tail -1
}

LATEST="$(latest_saved)"
LATEST="${LATEST:-0}"

while [ "$LATEST" -lt "$SUPERBATCH" ]; do
  LOG="$OUTPUT_DIR/train_sb$((LATEST + 1))_$(date +%s).log"
  echo "=== Training $NET_ID (H=$HIDDEN_SIZE) superbatch $((LATEST + 1)) -> $SUPERBATCH (log: $LOG) ==="

  export START_SUPERBATCH=$((LATEST + 1))
  if [ "$LATEST" -gt 0 ]; then
    export LOAD_CHECKPOINT="$OUTPUT_DIR/$NET_ID-$LATEST"
  else
    unset LOAD_CHECKPOINT
  fi

  ./target/release/trainer 2>&1 | tee -a "$LOG"

  NEW="$(latest_saved)"
  NEW="${NEW:-0}"
  if [ "$NEW" -le "$LATEST" ]; then
    echo "=== No progress, retrying in 5 minutes ==="
    sleep 300
  else
    LATEST="$NEW"
    sleep 10
  fi
done
echo "=== DONE at $NET_ID-$LATEST ==="
