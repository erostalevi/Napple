#!/bin/sh
# Build and run t_xlate on a host without Metal (Linux): translator + text assertions only, Metal compile checks are skipped.
# usage: sh tests/t_xlate_linux.sh   (from the gfx directory or anywhere)
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/t_xlate_linux
mkdir -p "$out"
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-format-overflow -Wno-missing-field-initializers -fno-strict-aliasing \
  -x c -I"$here/include" -I"$here/src" -I"$here/tests" "$here/tests/t_xlate.m" -x c "$here/src/shader/maxwell_translate.c" -o "$out/t_xlate"
"$out/t_xlate"
