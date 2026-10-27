#!/bin/sh
# Linux build and run of the portable service tests (tests/test_svc_*.c): runtime/tests/linux/run.sh [name ...]  (default: all that link).
# Links the real kernel/ipc/threads objects against tests/linux/mach_shim.h; a test that needs a macOS-only file fails to build and is skipped.
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build/linux
COMMON="tests/linux/ipc_mock.c src/ipc.c src/kernel.c src/threads.c src/ksession.c src/kmsg.c src/kcode.c src/timescale.c tests/host/host_stubs.c tests/host/host_nomodules.c"
CF="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-result -include tests/linux/mach_shim.h -Isrc -Isrc/services -Itests/host -pthread"
names="$*"; [ -z "$names" ] && names=$(ls tests/test_svc_*.c | sed 's|tests/test_svc_||; s|\.c$||')
fail=0; built=0; skipped=""
for n in $names; do
  t=tests/test_svc_$n.c
  # every service source the test #includes is left out of the link; the other service objects the test needs are the *_core.c ones
  inc=$(grep -o 'services/[A-Za-z0-9_]*\.c' "$t" | sort -u)
  cores=""; for f in src/services/*_core.c src/services/romfs_map.c src/services/hz_result.c; do [ -f "$f" ] || continue; skip=0; for i in $inc; do [ "src/$i" = "$f" ] && skip=1; done; [ $skip = 0 ] && cores="$cores $f"; done
  extra=""; if grep -q 'services/audio\.c' "$t"; then extra="$(ls ../audio/src/*.c | grep -v engine_ios) -I../audio/include -I../audio/src -I../audio/tests/linux/include -I/usr/include/opus -lopus -D_GNU_SOURCE"; fi
  if grep -q 'services/vi\.c' "$t"; then extra="$extra -I../gfx/include"; fi
  if grep -q 'services/hid\.c' "$t"; then extra="$extra src/input_script.c ../input/src/bindings.c ../input/src/hid_shm.c ../input/src/input.c ../input/src/motion.c ../input/src/names.c ../input/src/gesture.c ../input/tests/linux/ng_stub.c -I../input/include -I../input/src -D_GNU_SOURCE"; fi
  if cc $CF $extra "$t" $cores $COMMON -lm -Wl,--allow-multiple-definition -o build/linux/t_$n 2>build/linux/$n.log; then
    built=$((built+1)); if ! ./build/linux/t_$n >build/linux/$n.out 2>&1; then echo "FAIL $n"; tail -5 build/linux/$n.out; fail=1; else tail -1 build/linux/$n.out; fi
  else skipped="$skipped $n"; fi
done
echo "built+ran: $built, did not build on Linux:$skipped"
python3 -I tests/linux/check_known_calls.py || fail=1
exit $fail
