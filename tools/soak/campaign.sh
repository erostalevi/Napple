#!/bin/bash
# campaign.sh [runtime] [out]: the b7 overnight campaign. 20 starts of 45 s for every target, then a 20-minute soak (7 slices of 170 s) for LAS, SMO, MK8D base and update.
# Every run is its own `gamelock.py --` call, so the FIFO lock is released between runs and other Mac sessions get their turns.
HERE="$(cd "$(dirname "$0")" && pwd)"
RT="${1:-$HERE/../../runtime/build/napple-runtime}"; OUT="${2:-/private/tmp/napple-soak}"
for t in ${TARGETS:-acnh-update mk8d-base mk8d-update las smo}; do
  python3 "$HERE/soak.py" run "$t" --runs "${RUNS:-20}" --seconds 45 --runtime "$RT" --out "$OUT"
done
for t in ${SOAKS:-las smo mk8d-base mk8d-update}; do
  python3 "$HERE/soak.py" soak "$t" --minutes 20 --slice 170 --runtime "$RT" --out "$OUT"
done
