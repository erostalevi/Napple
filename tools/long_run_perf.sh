#!/bin/bash
# tools/long_run_perf.sh <runtime-binary> <data-dir> <seconds> <outdir> [extra runtime args...]
# Headless long run with NAPPLE_PERF_LOG=1 plus tools/long_monitor.sh (RSS/threads/CPU every 5 s), then a summary: stalled 5 s windows (gap > 150 ms), worst gap,
# pipelines built, blocking build ms (frame-path compile time), RSS first/last/max. Uses the default save root. For slowdown hunts on the first minutes vs. later ones.
set -uo pipefail
BIN="$1"; DIR="$2"; SECS="$3"; OUT="$4"; shift 4; mkdir -p "$OUT"; HERE=$(cd "$(dirname "$0")" && pwd)
NAPPLE_PERF_LOG=1 "$BIN" "$DIR" --headless --seconds "$SECS" "$@" >"$OUT/run.log" 2>&1 & PID=$!
"$HERE/long_monitor.sh" $PID "$OUT/mon.log" 5 & MON=$!
wait $PID; RC=$?; wait $MON 2>/dev/null
echo "exit $RC"
grep '\[perf\]' "$OUT/run.log" | awk '{n++; if ($5+0 > 150) s++; if ($5+0 > m) m = $5+0; if ($3+0 < lo || n == 1) lo = $3+0; split($0, a, "pipes\\+"); split(a[2], b, " "); p += b[1]; split($0, c, "blockbuild_ms\\+"); split(c[2], d, " "); bb += d[1]}
  END { printf "windows %d  stalled(>150ms) %d  worst gap %.0f ms  min fps %.1f  pipelines built %d  blocking build %d ms\n", n, s, m, lo, p, bb }'
awk 'NR > 1 && $2 ~ /^[0-9]+$/ { if (!f) f = $2; l = $2; if ($2 > mx) mx = $2 } END { printf "rss first %.0f MB  last %.0f MB  max %.0f MB\n", f/1024, l/1024, mx/1024 }' "$OUT/mon.log"
