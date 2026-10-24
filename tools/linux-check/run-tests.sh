#!/bin/sh
# Build and run runtime/tests/test_svc_*.c on Linux against ipc_mock.c. Every runtime/src/services/*_core.c is linked into each test
# (the *_core.c files are the portable logic and must not include macOS headers). A test #includes the service glue it exercises.
# Exit status is non-zero if any test fails to build or fails.
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); out=${TMPDIR:-/tmp}/napple-linux-check; mkdir -p "$out"
CC=${CC:-gcc}
FLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -include $here/compat.h -I$here/include -I$root/runtime/src -I$root/runtime/src/services -I$root/runtime/tests -I$root/input/include -I$root/audio/include -I$root/gfx/include -I$root/frontend/include"
[ $# -gt 0 ] && tests="$*" || tests=$(ls "$root"/runtime/tests/test_svc_*.c 2>/dev/null)
rc=0; n=0
for t in $tests; do
  name=$(basename "$t" .c)
  srcs=$(ls "$root"/runtime/src/services/*_core.c 2>/dev/null)
  if ! $CC $FLAGS "$t" $srcs "$here/ipc_mock.c" -lm -lpthread -o "$out/$name" 2> "$out/$name.log"; then echo "BUILD FAIL $name"; head -20 "$out/$name.log"; rc=1; continue; fi
  [ -s "$out/$name.log" ] && { echo "warnings in $name:"; head -10 "$out/$name.log"; }
  echo "== $name"; "$out/$name" || { echo "TEST FAIL $name"; rc=1; }
  n=$((n+1))
done
echo "ran $n test binaries"
exit $rc
