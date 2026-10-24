#!/bin/zsh
# usage: apptest.sh <logname> <app path> <seconds> <synth-delay> <keyspecs...>
# Launches the packaged .app through LaunchServices (the double-click path) and injects real global key events (HID tap) into whatever is frontmost.
cd "$(dirname "$0")/.." || exit 1
name=$1; app=$2; secs=$3; delay=$4; shift 4
got=0
for i in {1..300}; do mkdir work/gpu.lock 2>/dev/null && { got=1; break; }; sleep 3; done
[ $got = 1 ] || { echo 'could not get work/gpu.lock' >&2; exit 3; }
trap 'rmdir work/gpu.lock 2>/dev/null' EXIT
rm -f work/logs/$name.log work/logs/$name.err
extra=(); [ -n "$SCRIPT" ] && extra=(--env "NAPPLE_INPUT_SCRIPT=$SCRIPT")
open -n -W $extra --env NAPPLE_INPUT_LOG=1 --env NAPPLE_AUDIO_VOLUME=0.05 --env NAPPLE_SCREENSHOT=$PWD/work/shots/$name --env NAPPLE_SCREENSHOT_FRAMES=${SHOT_FRAMES:-2400,3000,3600} --stdout $PWD/work/logs/$name.log --stderr $PWD/work/logs/$name.log "$app" --args --save-root /tmp/napple-testroot --seconds $secs &
sleep 4
if [ $# -gt 0 ]; then work/scratch/synth_keys 0 $delay "$@" > work/logs/$name.synth 2>&1; fi
wait
