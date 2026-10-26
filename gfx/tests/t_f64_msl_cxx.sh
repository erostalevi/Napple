#!/bin/sh
# Compile the emitted MSL text of the double library as C++ (MSL is C++14-based) and compare it with the C build on random vectors.
# Needs g++ (or c++). Proves the stringified source is well-formed and typed; the real Metal compile + GPU run is t_f64.m (Mac).
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/t_f64_msl_cxx
mkdir -p "$out"
cat > "$out/dump.c" <<'EOT'
#include <stdio.h>
#include "shader/soft_double_msl.h"
int main(void) {
    for (const char *q = sd_msl_source; *q; q++) {
        putchar(*q);
        if (*q == '}' && q[1] == ' ') putchar('\n');
    }
    putchar('\n');
    return 0;
}
EOT
${CC:-gcc} -std=gnu11 -I"$here/src" "$out/dump.c" -o "$out/dump"
"$out/dump" > "$out/msl_dump.inc"
${CXX:-g++} -std=c++14 -O1 -Wall -Wextra -Wno-unused-function -Wno-misleading-indentation -I"$out" -I"$here/src" -x c++ "$here/tests/t_f64_msl_cxx.cpp" -o "$out/t_f64_msl_cxx"
"$out/t_f64_msl_cxx"
