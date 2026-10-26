#!/bin/bash
# tools/legacy/flaky_boot.sh <runtime-binary> <data-dir> <runs> [seconds] <outdir>
# Runs a headless boot N times with the given runtime and prints one line per run: exit code, first FAIL/crash line, time of exit.
# Uses the default save root (it holds system/fonts; an empty --save-root crashes in pl.c); set SAVE_ROOT=dir to override.
# Sequential on purpose (one GPU user at a time). Used to tell a flaky guest Break from a build-specific one.
set -uo pipefail
BIN="$1"; DIR="$2"; N="$3"; SECS="${4:-12}"; OUT="${5:-/tmp/flaky_boot}"; mkdir -p "$OUT"
TAG=$(basename "$(dirname "$DIR")")-$(basename "$(dirname "$(dirname "$BIN")")")
for i in $(seq 1 "$N"); do
  S=$(date +%s.%N 2>/dev/null || date +%s)
  "$BIN" "$DIR" --headless --seconds "$SECS" ${SAVE_ROOT:+--save-root "$SAVE_ROOT"} >"$OUT/$TAG-$i.log" 2>&1; RC=$?
  echo "$TAG run $i exit $RC | $(grep -m1 -E 'FAIL:|SIGSEGV|crash' "$OUT/$TAG-$i.log" | cut -c1-150)"
done
