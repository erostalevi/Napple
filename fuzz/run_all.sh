#!/usr/bin/env bash
# Runs every cargo-fuzz target for a fixed budget and writes a summary.
# usage: fuzz/run_all.sh [seconds-per-target=300] [parallel-jobs=2] [log-dir=fuzz/logs]
# Needs: rustup nightly + cargo-fuzz (see docs/guides/fuzzing.md). Exit 1 when any target crashed.
set -u
cd "$(dirname "$0")"
secs=${1:-300}
jobs=${2:-2}
logs=${3:-logs}
mkdir -p "$logs"
[ -d corpus ] || cargo run --release --bin mkcorpus >/dev/null
targets=$(cargo +nightly fuzz list)
run_one() {
  t=$1; secs=$2; logs=$3
  mkdir -p "corpus/$t"
  cargo +nightly fuzz run "$t" -- -max_total_time="$secs" -timeout=20 -rss_limit_mb=2048 -max_len=262144 >"$logs/$t.log" 2>&1
  echo "$t exit=$?" >>"$logs/summary.txt"
}
export -f run_one
: >"$logs/summary.txt"
printf '%s\n' $targets | xargs -P "$jobs" -I{} bash -c 'run_one {} '"$secs $logs"
cat "$logs/summary.txt"
! grep -qv 'exit=0' "$logs/summary.txt"
