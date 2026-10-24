#!/bin/bash
# tools/boot_test.sh <titleid> [seconds] [extra runtime args...]
# Runs the runtime on work/games/<titleid>/<newest build> headless for N seconds (default 20), logging to work/logs and
# writing the compat stage file work/compat/<titleid>/<buildid>/runtime-boot.json (docs/compat-format.md).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TID="${1:?usage: boot_test.sh <titleid> [seconds]}"; SECS="${2:-20}"; shift; shift 2>/dev/null || true
DIR=$(ls -dt "$ROOT/work/games/$TID"/*/ 2>/dev/null | head -1); DIR="${DIR%/}"
[ -n "$DIR" ] || { echo "no data directory for $TID (run tools/prepare_game.sh)"; exit 2; }
BUILD=$(basename "$DIR"); STAMP=$(date +%Y%m%d-%H%M%S)
LOGDIR="$ROOT/work/logs"; mkdir -p "$LOGDIR"
LOG="$LOGDIR/$TID-$STAMP.log"; TRACE="$LOGDIR/$TID-$STAMP.trace.jsonl"
make -C "$ROOT/runtime" >/dev/null || { echo "runtime build failed"; exit 2; }
export NAPPLE_EVIDENCE="work/logs/$TID-$STAMP.log"
"$ROOT/runtime/build/napple-runtime" "$DIR" --headless --seconds "$SECS" --trace "$TRACE" \
  --summary "$ROOT/work/compat/$TID/$BUILD/runtime-boot.json" --save-root "$ROOT/work/saves" "$@" >"$LOG" 2>&1
RC=$?
echo "exit $RC; log $LOG"; tail -5 "$LOG"
cat "$ROOT/work/compat/$TID/$BUILD/runtime-boot.json" 2>/dev/null | head -c 600; echo
