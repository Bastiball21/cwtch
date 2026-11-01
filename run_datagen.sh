#!/bin/bash
# ---------------------------------------------------------------------------
# Cwtch Datagen Launcher
# ---------------------------------------------------------------------------

set -u

# --- Configurable ----------------------------------------------------------
BINARY="${BINARY:-./cwtch}"
OUTPUT_BASE="${OUTPUT_BASE:-/mnt/d/cwtch_datagen_standard}"
TARGET_POSITIONS="${TARGET_POSITIONS:-500000000}"
THREADS="${THREADS:-28}"

# MODE: "frc", "dfrc", a path to an .epd/.pgn book, or empty for random openings
MODE="${1:-}"

OUTPUT_DIR="$OUTPUT_BASE/data"
LOG_DIR="$OUTPUT_BASE/logs"

# --- Validate --------------------------------------------------------------
if [ ! -x "$BINARY" ]; then
    echo "Error: '$BINARY' not found or not executable"
    exit 1
fi

if [ "$THREADS" -lt 1 ]; then
    echo "Error: THREADS must be >= 1"
    exit 1
fi

if [ "$THREADS" -gt "$TARGET_POSITIONS" ]; then
    echo "Error: more threads than target positions"
    exit 1
fi

# Distribute: first REMAINDER threads get one extra position each
BASE_PER_THREAD=$((TARGET_POSITIONS / THREADS))
REMAINDER=$((TARGET_POSITIONS % THREADS))

mkdir -p "$OUTPUT_DIR" "$LOG_DIR"
rm -f "$LOG_DIR"/thread_*.log

# --- Signal handling -------------------------------------------------------
declare -a PIDS=()
CLEANED=0

cleanup() {
    [ "$CLEANED" -eq 1 ] && return
    CLEANED=1

    echo ""
    echo "[$(date '+%F %T')] Shutdown requested — signalling ${#PIDS[@]} datagen threads..."
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null
    done

    # Give children time to flush zstd, end the stream, and close cleanly.
    sleep 5

    # Force-kill any that didn't exit on their own (should be zero).
    STRAGGLERS=0
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            STRAGGLERS=$((STRAGGLERS + 1))
            kill -9 "$pid" 2>/dev/null
        fi
    done

    wait 2>/dev/null

    if [ "$STRAGGLERS" -eq 0 ]; then
        echo "[$(date '+%F %T')] All threads exited cleanly."
    else
        echo "[$(date '+%F %T')] Force-killed $STRAGGLERS straggler(s)."
    fi

    # Print whatever totals we can salvage from the logs so far.
    print_final_totals
    exit 130
}
trap cleanup INT TERM HUP

# --- Parse a single thread's log -------------------------------------------
# Echoes: "<positions> <games> <speed> <dup> <fallback> <is_done>"
# is_done = 1 if the thread printed "datagen: done.", else 0.
parse_thread_log() {
    local log="$1"
    [ -f "$log" ] || { echo "0 0 0 0 0 0"; return; }

    local done_line
    done_line=$(grep -E '^datagen: done\. [0-9]+ positions' "$log" 2>/dev/null | tail -n 1)

    local pos=0 games=0 speed=0 is_done=0
    if [ -n "$done_line" ]; then
        # "datagen: done. 13571 positions 161 games written to ..."
        pos=$(awk '{print $3}' <<< "$done_line")
        games=$(awk '{print $5}' <<< "$done_line")
        is_done=1
    else
        # Fall back to the last periodic progress line.
        local prog_line
        prog_line=$(grep -E '^datagen: [0-9]+/[0-9]+ positions' "$log" 2>/dev/null | tail -n 1)
        if [ -n "$prog_line" ]; then
            # "datagen: N/M positions (P%) G games S pos/s dup D% fallback F [...]"
            pos=$(awk '{ split($2, a, "/"); print a[1] }' <<< "$prog_line")
            games=$(awk '{print $5}' <<< "$prog_line")
            speed=$(awk '{print $7}' <<< "$prog_line")
        fi
    fi

    # Final stats line (only present on clean exit):
    # "datagen: final stats — dup rejects 505, fallback moves 0"
    local dup=0 fb=0 stats_line
    stats_line=$(grep -E '^datagen: final stats' "$log" 2>/dev/null | tail -n 1)
    if [ -n "$stats_line" ]; then
        dup=$(sed -n 's/.*dup rejects \([0-9]*\).*/\1/p' <<< "$stats_line")
        fb=$(sed -n 's/.*fallback moves \([0-9]*\).*/\1/p' <<< "$stats_line")
        [ -z "$dup" ] && dup=0
        [ -z "$fb" ]  && fb=0
    fi

    echo "$pos $games $speed $dup $fb $is_done"
}

# --- Aggregate totals across all thread logs -------------------------------
# Sets POS_SUM, GAMES_SUM, SPEED_SUM, DUP_SUM, FB_SUM, DONE_COUNT.
aggregate_logs() {
    POS_SUM=0; GAMES_SUM=0; SPEED_SUM=0
    DUP_SUM=0; FB_SUM=0; DONE_COUNT=0

    for log in "$LOG_DIR"/thread_*.log; do
        [ -f "$log" ] || continue
        read -r p g s d f done_flag < <(parse_thread_log "$log")
        POS_SUM=$((POS_SUM + p))
        GAMES_SUM=$((GAMES_SUM + g))
        SPEED_SUM=$((SPEED_SUM + s))
        DUP_SUM=$((DUP_SUM + d))
        FB_SUM=$((FB_SUM + f))
        [ "$done_flag" -eq 1 ] && DONE_COUNT=$((DONE_COUNT + 1))
    done
}

# --- Print final totals ----------------------------------------------------
print_final_totals() {
    aggregate_logs

    echo ""
    echo "============================================================"
    echo "  Datagen summary"
    echo "============================================================"
    printf "  Positions : %'d / %'d (%.2f%%)\n" \
        "$POS_SUM" "$TARGET_POSITIONS" \
        "$(awk -v p="$POS_SUM" -v t="$TARGET_POSITIONS" 'BEGIN { printf "%.2f", (p/t)*100 }')"
    printf "  Games     : %'d\n" "$GAMES_SUM"
    printf "  Dup rejects: %'d\n" "$DUP_SUM"
    printf "  Fallback  : %'d\n" "$FB_SUM"
    printf "  Threads   : %d/%d finished\n" "$DONE_COUNT" "$THREADS"
    echo "============================================================"
    echo ""
}

# --- Launch ----------------------------------------------------------------
echo "[$(date '+%F %T')] Starting $THREADS-thread datagen"
echo "[$(date '+%F %T')] Target: $TARGET_POSITIONS positions"
echo "[$(date '+%F %T')] Mode:   ${MODE:-<random openings>}"
echo "[$(date '+%F %T')] Output: $OUTPUT_DIR"
echo ""

for i in $(seq 1 "$THREADS"); do
    if [ "$i" -le "$REMAINDER" ]; then
        POS=$((BASE_PER_THREAD + 1))
    else
        POS=$BASE_PER_THREAD
    fi

    "$BINARY" "datagen $OUTPUT_DIR $POS $MODE" \
        >/dev/null 2>"$LOG_DIR/thread_${i}.log" &
    PIDS+=($!)
done

echo "[$(date '+%F %T')] Launched ${#PIDS[@]} threads."

# --- Progress loop ---------------------------------------------------------
while true; do
    ALIVE=0
    for pid in "${PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            ALIVE=$((ALIVE + 1))
        fi
    done

    aggregate_logs

    PCT=$(awk -v p="$POS_SUM" -v t="$TARGET_POSITIONS" 'BEGIN { printf "%.2f", (p/t)*100 }')
    REMAINING=$((TARGET_POSITIONS - POS_SUM))
    [ "$REMAINING" -lt 0 ] && REMAINING=0

    if [ "$SPEED_SUM" -gt 0 ]; then
        ETA_SEC=$((REMAINING / SPEED_SUM))
        ETA_TIME=$(date -d "+${ETA_SEC} seconds" '+%m-%d %H:%M' 2>/dev/null || echo "N/A")
    else
        ETA_TIME="--"
    fi

    printf '\r[%s] %d/%d (%.2f%%) | %d games | %d pos/s | dup %d | fb %d | ETA %s | threads %d/%d' \
        "$(date '+%H:%M:%S')" \
        "$POS_SUM" "$TARGET_POSITIONS" "$PCT" \
        "$GAMES_SUM" "$SPEED_SUM" \
        "$DUP_SUM" "$FB_SUM" \
        "$ETA_TIME" "$ALIVE" "$THREADS"

    if [ "$ALIVE" -eq 0 ]; then
        echo ""
        echo ""
        echo "[$(date '+%F %T')] All threads finished."
        print_final_totals
        break
    fi

    sleep 2
done

wait 2>/dev/null