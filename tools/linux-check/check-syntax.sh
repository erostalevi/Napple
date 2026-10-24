#!/bin/sh
# Syntax-check the portable service sources with gcc and clang. Usage: tools/linux-check/check-syntax.sh [file.c ...]
# Default: every runtime/src/services/*.c. Exit status is non-zero if any file fails.
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
FLAGS="-std=gnu11 -Wall -Wextra -Wno-unused-parameter -fsyntax-only -include $here/compat.h -I$here/include -I$root/runtime/src -I$root/runtime/src/services -I$root/input/include -I$root/audio/include -I$root/gfx/include -I$root/frontend/include"
[ $# -gt 0 ] && files="$*" || files=$(ls "$root"/runtime/src/services/*.c)
# Known Darwin-only constructs that no stub header can express: hid.c reads the arm64 Darwin ucontext (mcontext_t->__ss) in its
# NAPPLE_HID_TRACE fault emulator. Errors that match are tolerated for that file; any other error still fails the check.
tolerated() { case "$1" in */hid.c) grep -v -E "mcontext_t|__ss|uc_mcontext" ;; *) cat ;; esac; }
rc=0
for f in $files; do
  # host_info.c / nvdrv.c / fs.c / hid.c include macOS frameworks beyond the stubs; they are reported, not hidden
  case "$f" in */fs.c|*/nvdrv.c) echo "skip $f (owned by another session; needs macOS headers)"; continue;; esac
  for cc in gcc clang; do
    command -v $cc >/dev/null 2>&1 || continue
    out=$($cc $FLAGS "$f" 2>&1); st=$?
    left=$(echo "$out" | tolerated "$f" | grep -E "error:|fatal error:")
    if [ $st -ne 0 ] && [ -n "$left" ]; then echo "FAIL ($cc) $f"; echo "$left" | head -20; rc=1; continue; fi
    warn=$(echo "$out" | grep -E "warning:" | head -10)
    [ -n "$warn" ] && { echo "WARN ($cc) $f"; echo "$warn"; }
  done
done
[ $rc -eq 0 ] && echo "syntax check: all clean"
exit $rc
