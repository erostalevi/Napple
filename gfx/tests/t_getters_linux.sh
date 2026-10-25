#!/bin/sh
# Build and run t_getters_linux on a host without Metal (Linux): set -> get round trips of the NVN getters.
# On macOS it delegates to the Makefile target (the same binary `make -C gfx test` runs).
# usage: sh tests/t_getters_linux.sh
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
if [ "$(uname -s)" = Darwin ]; then
  make -C "$here" build/tests/t_getters_linux >/dev/null && exec "$here/build/tests/t_getters_linux"
fi
out=${TMPDIR:-/tmp}/t_getters_linux
mkdir -p "$out"
${CC:-gcc} -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-misleading-indentation -Wno-missing-field-initializers -fno-strict-aliasing -DNVN_IMPL_SECTION=\"napple_nvn\" \
  -I"$here/include" -I"$here/src" "$here/tests/t_getters_linux.c" -Wl,--unresolved-symbols=ignore-all -o "$out/t_getters_linux"
"$out/t_getters_linux"
