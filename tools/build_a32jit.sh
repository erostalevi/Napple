#!/bin/sh
# Lifts the AArch32 modules of a game data directory to AArch64 and builds <data dir>/modules/a32jit.dylib (mixed mode: the runtime falls back to
# the interpreter wherever the image has no block). No code is generated at run time; the dylib is an ordinary Mach-O that can be signed.
#   tools/build_a32jit.sh <data dir> [work dir]
set -e
DATA=${1:?usage: build_a32jit.sh <data dir> [work dir]}
WORK=${2:-${TMPDIR:-/tmp}/a32jit_build}
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODS="$DATA/modules"
mkdir -p "$WORK"
cc -std=gnu11 -O2 -I"$HERE/runtime/src/a32" -o "$WORK/a32lift" "$HERE/runtime/src/a32jit/a32lift.c" "$HERE/runtime/src/a32jit/a32lift_main.c" \
   "$HERE/runtime/src/a32/a32_module.c" "$HERE/runtime/src/a32/a32_decode_arm.c" "$HERE/runtime/src/a32/a32_decode_thumb.c"
rm -f "$WORK"/j.*
"$WORK/a32lift" "$MODS" "$WORK/j"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
sed 's/$/.o/' "$WORK/j.files" > "$WORK/objs.txt"
cat "$WORK/j.files" | xargs -P "$JOBS" -I{} clang -c -target arm64-apple-macos13 -march=armv8.4-a {} -o {}.o
clang -dynamiclib -target arm64-apple-macos13 -Wl,-x -o "$WORK/a32jit.dylib" @"$WORK/objs.txt"
cp "$WORK/a32jit.dylib" "$MODS/a32jit.dylib"
cp "$WORK/j.manifest" "$MODS/a32jit.manifest"
ls -la "$MODS/a32jit.dylib"
