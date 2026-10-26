#!/bin/sh
# Syntax-check runtime/src/movie.c (macOS: NEON, libvpx, mach clock) with stub headers kept here. Proves only that the C is well formed against
# the declared interfaces; the real build and the SDK library behaviour are Mac checks. Usage: tools/linux-check/check-movie.sh
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
rc=0
for cc in gcc clang; do
  command -v $cc >/dev/null 2>&1 || continue
  out=$($cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fsyntax-only -include "$here/compat.h" -include "$here/movie/movie_compat.h" -I"$here/movie" -I"$here/include" \
        -I"$root/runtime/src" -I"$root/runtime/src/services" -I"$root/gfx/include" "$root/runtime/src/movie.c" 2>&1) || rc=1
  [ -n "$out" ] && { echo "== $cc"; echo "$out" | grep -E "error|warning" | head -20; }
done
[ $rc -eq 0 ] && echo "movie.c syntax check: clean"
exit $rc
