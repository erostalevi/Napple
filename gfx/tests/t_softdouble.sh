#!/bin/sh
# Build and run t_softdouble (host-double verification of the Maxwell f64 emulation). Linux or Mac; T_SD_N=<n> sets the random iterations.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/t_softdouble
mkdir -p "$out"
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -frounding-math -fno-fast-math -ffp-contract=off -I"$here/src" "$here/tests/t_softdouble.c" -lm -o "$out/t_softdouble"
"$out/t_softdouble"
