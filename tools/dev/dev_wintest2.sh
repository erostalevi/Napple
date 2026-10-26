#!/bin/zsh
# usage: wintest2.sh <logname> <datadir> <seconds> <shot-at-secs> <outpng> <synth-delay> <keyspecs...>
cd "$(dirname "$0")/../.." || exit 1
name=$1; dir=$2; secs=$3; shotat=$4; png=$5; delay=$6; shift 6
got=0
for i in {1..300}; do mkdir work/gpu.lock 2>/dev/null && { got=1; break; }; sleep 3; done
[ $got = 1 ] || { echo 'could not get work/gpu.lock' >&2; exit 3; }
trap 'rmdir work/gpu.lock 2>/dev/null' EXIT
env NAPPLE_FE_VIEWSHOT=$png:$shotat ${OVERLAY_TAB:+NAPPLE_FE_OVERLAY=$OVERLAY_TAB} NAPPLE_INPUT_LOG=1 NAPPLE_AUDIO_DEVICE=1 NAPPLE_AUDIO_VOLUME=0.05 runtime/build/napple-runtime $dir --save-root work/testroot --seconds $secs > work/logs/$name.log 2>&1 &
rt=$!
sleep 3
work/scratch/synth_keys $rt $delay "$@" > work/logs/$name.synth 2>&1
wait $rt
