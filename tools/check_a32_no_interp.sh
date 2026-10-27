#!/bin/sh
# Proof that the shipped (release) runtime has no AArch32 interpreter and that the lifted image is a build product:
#   tools/check_a32_no_interp.sh [runtime binary] [data dir with modules/a32jit.dylib]
# 1. the binary must not contain the interpreter loop or the decoders (a32_run, a32_decode_arm, a32_decode_thumb, a32_thumb_is32);
# 2. it must contain the out-of-line operation semantics (a32_op_exec) and the lifted-image loader (a32jit_run);
# 3. no source file of the A32 runtime path may create executable memory (MAP_JIT, pthread_jit_write_protect, mprotect with PROT_EXEC);
# 4. if a data dir is given, modules/a32jit.dylib and a32jit.manifest must exist (written by `napple build`, not at run time).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$HERE/runtime/build/napple-runtime}
DATA=$2
fail=0
syms=$(nm "$BIN" 2>/dev/null)
for s in a32_run a32_decode_arm a32_decode_thumb a32_thumb_is32; do
    if echo "$syms" | grep -q " _$s\$"; then echo "FAIL: $BIN contains $s (interpreter)"; fail=1; fi
done
for s in a32_op_exec a32jit_run; do
    if ! echo "$syms" | grep -q " _$s\$"; then echo "FAIL: $BIN lacks $s"; fail=1; fi
done
if grep -n "MAP_JIT\|pthread_jit_write_protect\|PROT_EXEC" "$HERE"/runtime/src/a32/*.c "$HERE"/runtime/src/a32jit/*.c | grep -v "^\s*//" | grep -v "no PROT_EXEC"; then
    echo "FAIL: the A32 runtime path creates executable memory"; fail=1
fi
if [ -n "$DATA" ]; then
    for f in a32jit.dylib a32jit.manifest; do
        [ -s "$DATA/modules/$f" ] || { echo "FAIL: $DATA/modules/$f missing (napple build writes it)"; fail=1; }
    done
fi
[ $fail = 0 ] && echo "ok: no AArch32 interpreter in $BIN; lifted image is a build product"
exit $fail
