# 2026-10-25 ios-sim-titles: the four titles on the iOS Simulator

Branch claude/ios-sim-2026-10-25-titles. Details and the table of results: `docs/ios.md`, last section.

- Verified on the Simulator (iPhone 17, iOS 26.1): LAS boots to its title screen with video, audio and the touch controller; SMO reaches its Start menu; MK8D base (AArch32) runs the interpreter for about 15 s and then hits the Simulator GPU's 32-byte colour-attachment limit; MK8D with the latest update is a 64-bit build that stops at `time:u:300`; ACNH 3.0.3 (delta RomFS) runs 100 s without a crash and without a first frame (same as macOS at the time of the run; ac-render has fixes in flight).
- Fixes: a32 sources in build_ios.sh; libvpx for the Simulator in build_ios_deps.sh; depth textures Private on iOS (gfx/src/mtl_tex.m); overlapping Metal buffers for pools above maxBufferLength (gfx/src/mtl_core.m); Simulator heap cap 4 GiB (runtime/src/memory.c); AArch32 titles skipped by `dylibize` (napple-build ios.rs); IPA written with `ditto --norsrc --noextattr --noacl`.
- IPA of LAS (`--target ios --no-sign --ipa`) inspected: structure and platform fields fine.
- Not verified: anything on a device (signing, dyld, memory limits, the 6 GiB window with LAS).

## Update 2026-10-26
- `time:u` cmd 300 implemented (services/time.c, owner approved the widening); untested against MK8D 3.x because game data was removed from `work/games` by another session (disk).
- Touch delivery on the Simulator not working from the tool (see docs/ios.md follow-up notes); LAS driven to the file-select screen with NAPPLE_INPUT_SCRIPT.
- Not done: gameplay screenshots, RSS numbers, ACNH/MK8D re-tests on other branches, a32jit.dylib packaging, Studio checks. Blocked by disk (2.6 GB free) and missing game data.
