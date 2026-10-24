#!/bin/zsh
# usage: gpurun.sh <logfile> <cmd...>  ; acquires work/gpu.lock
cd "$(dirname "$0")/.." || exit 1
log=$1; shift
got=0
for i in {1..300}; do mkdir work/gpu.lock 2>/dev/null && { got=1; break; }; sleep 3; done
[ $got = 1 ] || { echo 'could not get work/gpu.lock' >&2; exit 3; }
trap 'rmdir work/gpu.lock 2>/dev/null' EXIT
"$@" > $log 2>&1
