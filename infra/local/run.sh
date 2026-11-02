#!/usr/bin/env bash
# Local test stack of the Napple game servers, native (no Docker): builds napple-online + napple-room from this checkout and runs them on
# 127.0.0.1 with a throwaway data dir, then prints the env that points a Napple runtime at them.
#   infra/local/run.sh [start] [--release] [--no-build] [--bin-dir DIR] [--stub]   build (debug by default) and start in the background
#   infra/local/run.sh stop | status | env | logs
# --bin-dir DIR  use napple-online / napple-room binaries from DIR instead of building
# --stub         use infra/local/stub-server.py instead of the real binaries (tests the plumbing before the crates exist)
# Ports (env): ONLINE_HTTP_PORT=18080 ROOM_PORT=31415 ROOM_HTTP_PORT=18081
# State: $NAPPLE_LOCAL_STACK_DIR (default $TMPDIR/napple-local-stack): data/, logs/, *.pid. `stop` removes the data dir unless KEEP_DATA=1.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
state="${NAPPLE_LOCAL_STACK_DIR:-${TMPDIR:-/tmp}/napple-local-stack}"
state="${state%/}"
ONLINE_HTTP_PORT="${ONLINE_HTTP_PORT:-18080}"
ROOM_PORT="${ROOM_PORT:-31415}"
ROOM_HTTP_PORT="${ROOM_HTTP_PORT:-18081}"

cmd="start" profile="debug" build=1 bin_dir="" stub=0
while [ $# -gt 0 ]; do
  case "$1" in
    start|stop|status|env|logs) cmd="$1" ;;
    --release) profile="release" ;;
    --no-build) build=0 ;;
    --bin-dir) bin_dir="$2"; build=0; shift ;;
    --stub) stub=1; build=0 ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

print_env() {
  cat <<ENV
# Point a Napple runtime (or a built game app) at the local servers:
export NAPPLE_NET=host
export NAPPLE_ONLINE_SERVER=http://127.0.0.1:$ONLINE_HTTP_PORT
export NAPPLE_ROOM=127.0.0.1:$ROOM_PORT
ENV
}

alive() { [ -f "$state/$1.pid" ] && kill -0 "$(cat "$state/$1.pid")" 2>/dev/null; }

stop_one() {
  local name="$1" pid
  alive "$name" || { rm -f "$state/$name.pid"; return 0; }
  pid="$(cat "$state/$name.pid")"
  kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  kill -0 "$pid" 2>/dev/null && { echo "$name ($pid) ignored SIGTERM, killing"; kill -KILL "$pid" 2>/dev/null || true; }
  rm -f "$state/$name.pid"
  echo "stopped $name ($pid)"
}

wait_healthy() {
  local name="$1" url="$2"
  for _ in $(seq 1 100); do
    if curl -fsS --max-time 1 "$url" >/dev/null 2>&1; then echo "$name healthy: $url"; return 0; fi
    alive "$name" || { echo "$name exited; last log lines:" >&2; tail -n 20 "$state/logs/$name.log" >&2; return 1; }
    sleep 0.2
  done
  echo "$name not healthy after 20 s: $url" >&2; return 1
}

# cargo on PATH, else the rustup toolchain's (Homebrew's rustup installs no proxies: its toolchain dir must be on PATH for rustc too).
find_cargo() {
  if command -v cargo >/dev/null 2>&1; then command -v cargo
  elif command -v rustup >/dev/null 2>&1; then rustup which cargo
  else echo "cargo not found (install rustup)" >&2; return 1
  fi
}

case "$cmd" in
  env) print_env; exit 0 ;;
  logs) tail -n 50 "$state"/logs/*.log; exit 0 ;;
  status)
    for n in online room; do if alive "$n"; then echo "$n running (pid $(cat "$state/$n.pid"))"; else echo "$n stopped"; fi; done
    exit 0 ;;
  stop)
    stop_one online; stop_one room
    if [ "${KEEP_DATA:-0}" != 1 ]; then rm -rf "$state/data"; fi
    exit 0 ;;
esac

# start
if alive online || alive room; then echo "already running (infra/local/run.sh stop first)" >&2; exit 1; fi
mkdir -p "$state/data/online" "$state/data/room" "$state/logs"

if [ "$stub" = 1 ]; then
  online_cmd=(python3 "$here/stub-server.py" online)
  room_cmd=(python3 "$here/stub-server.py" room)
else
  if [ -z "$bin_dir" ]; then
    for c in napple-online napple-room; do
      [ -f "$root/crates/$c/Cargo.toml" ] || { echo "crates/$c is not in this checkout: merge its branch, or use --bin-dir DIR / --stub" >&2; exit 1; }
    done
    cargo="$(find_cargo)"
    PATH="$(dirname "$cargo"):$PATH"
    if [ "$build" = 1 ]; then
      flags=(); [ "$profile" = release ] && flags=(--release)
      (cd "$root" && "$cargo" build ${flags[@]+"${flags[@]}"} -p napple-online -p napple-room)
    fi
    bin_dir="${CARGO_TARGET_DIR:-$root/target}/$profile"
  fi
  online_cmd=("$bin_dir/napple-online" serve)
  room_cmd=("$bin_dir/napple-room" serve)
  for b in "${online_cmd[0]}" "${room_cmd[0]}"; do [ -x "$b" ] || { echo "missing binary $b" >&2; exit 1; }; done
fi

NAPPLE_ONLINE_HTTP="127.0.0.1:$ONLINE_HTTP_PORT" NAPPLE_ONLINE_PUBLIC_HOST=127.0.0.1 NAPPLE_ONLINE_DATA="$state/data/online" \
NAPPLE_ONLINE_VERBOSE="${NAPPLE_ONLINE_VERBOSE:-0}" \
  nohup "${online_cmd[@]}" >"$state/logs/online.log" 2>&1 &
echo $! > "$state/online.pid"
NAPPLE_ROOM_LISTEN="127.0.0.1:$ROOM_PORT" NAPPLE_ROOM_HTTP="127.0.0.1:$ROOM_HTTP_PORT" NAPPLE_ROOM_DATA="$state/data/room" RUST_LOG="${RUST_LOG:-info}" \
  nohup "${room_cmd[@]}" >"$state/logs/room.log" 2>&1 &
echo $! > "$state/room.pid"

ok=1
wait_healthy online "http://127.0.0.1:$ONLINE_HTTP_PORT/healthz" || ok=0
wait_healthy room "http://127.0.0.1:$ROOM_HTTP_PORT/healthz" || ok=0
if [ "$ok" = 0 ]; then "$0" stop >/dev/null; exit 1; fi
echo "logs: $state/logs/   stop: infra/local/run.sh stop"
print_env
