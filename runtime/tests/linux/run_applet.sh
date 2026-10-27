#!/bin/sh
# Linux build and run of the applet tests (test_svc_applet*.c), which #include applet.c and so need the stubs in applet_stubs.c: runtime/tests/linux/run_applet.sh [name ...]
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build/linux
COMMON="tests/linux/ipc_mock.c tests/linux/applet_stubs.c src/ipc.c src/kernel.c src/threads.c src/ksession.c src/kmsg.c src/kcode.c src/timescale.c tests/host/host_stubs.c tests/host/host_nomodules.c"
CF="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-result -include tests/linux/mach_shim.h -Isrc -Isrc/services -Itests/host -pthread -I../gfx/include -I../frontend/include"
names="$*"; [ -z "$names" ] && names=$(ls tests/test_svc_applet*.c | sed 's|tests/test_svc_||; s|\.c$||')
fail=0
for n in $names; do
  t=tests/test_svc_$n.c; [ -f "$t" ] || continue
  inc=$(grep -o 'services/[A-Za-z0-9_]*\.c' "$t" | sort -u)
  cores=""; for f in src/services/applet_core.c src/services/applet_lib_core.c; do skip=0; for i in $inc; do [ "src/$i" = "$f" ] && skip=1; done; [ $skip = 0 ] && cores="$cores $f"; done
  if cc $CF "$t" $cores $COMMON -lm -ldl -Wl,--allow-multiple-definition -o build/linux/t_$n 2>build/linux/$n.log; then
    if ! ./build/linux/t_$n >build/linux/$n.out 2>&1; then echo "FAIL $n"; tail -8 build/linux/$n.out; fail=1; else tail -1 build/linux/$n.out; fi
  else echo "BUILD FAIL $n"; grep -E "error|undefined" build/linux/$n.log | head; fail=1; fi
done
exit $fail
