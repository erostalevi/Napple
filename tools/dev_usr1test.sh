#!/bin/zsh
# usage: usr1test.sh <datadir> <runsecs> <usr1-at-secs> <logname> [more usr1 times...]   (env passes through)
cd "$(dirname "$0")/.." || exit 1
dir=$1; secs=$2; at=$3; name=$4; shift 4
got=0
for i in {1..300}; do mkdir work/gpu.lock 2>/dev/null && { got=1; break; }; sleep 3; done
[ $got = 1 ] || { echo 'could not get work/gpu.lock' >&2; exit 3; }
trap 'rmdir work/gpu.lock 2>/dev/null' EXIT
runtime/build/napple-runtime $dir --headless --seconds $secs --save-root work/testroot > work/logs/$name.log 2>&1 &
pid=$!
sleep $at; kill -USR1 $pid
for t in "$@"; do sleep $t; kill -USR1 $pid; done
wait $pid
