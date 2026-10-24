#!/bin/sh
# Generates build/gen/nvn_names.c (name table) and build/gen/nvn_unimpl.S (one stub per NVN name that reports the call and returns 0).
# usage: gen_nvn_tables.sh <names.txt> <outdir>
set -e
names="$1"; out="$2"
mkdir -p "$out"
{
  echo "/* generated from $(basename "$names") */"
  echo "#include <stddef.h>"
  echo "const char *const nvn_names[] = {"
  awk '{ printf "    \"%s\",\n", $1 }' "$names"
  echo "};"
  echo "const unsigned nvn_name_count = sizeof nvn_names / sizeof *nvn_names;"
} > "$out/nvn_names.c.tmp"
{
  echo "/* generated: unimplemented NVN entry stubs. x16 carries the index into the common handler. */"
  echo "    .text"
  echo "    .p2align 2"
  awk '{ printf "Lunimpl%d:\n    mov w16, #%d\n    b _nvn_unimpl_common\n", NR-1, NR-1 }' "$names"
  cat <<'ASM'
    .globl _nvn_unimpl_common
    .p2align 2
_nvn_unimpl_common:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov w0, w16
    bl _nvn_unimpl_report
    ldp x29, x30, [sp], #16
    mov x0, #0
    movi d0, #0
    ret
    .section __DATA,__const
    .globl _nvn_unimpl_thunk
    .p2align 3
_nvn_unimpl_thunk:
ASM
  awk '{ printf "    .quad Lunimpl%d\n", NR-1 }' "$names"
} > "$out/nvn_unimpl.S.tmp"
mv "$out/nvn_unimpl.S.tmp" "$out/nvn_unimpl.S"
mv "$out/nvn_names.c.tmp" "$out/nvn_names.c"
