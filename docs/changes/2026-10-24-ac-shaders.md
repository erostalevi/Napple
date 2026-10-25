# ac-shaders: Animal Crossing 3.0.3 "loading shaders" investigation + shader progress output (2026-10-24)

## Finding (measured)
The 3+ minute "loading shaders" screen is not shader work. Cold cache, 3.0.3 build (ff1d1c05670db602), headless 45 s: gfx stats frames 0 draws 0 pipelines 0 shaders 0 pending 0, `<cache>/shaders` stays empty: the game never submits a shader. The frontend text "First launch: shaders are compiled as the game needs them..." (runtime/src/app.m, not changed here) is static and stays up because the game never draws.
The game stalls on the CPU side: guest thread id 0x54 (main/boot thread, prio 44, core 0) spins on svc 0x18 WaitSynchronization(1 handle, timeout 0) ~25M times/s (1.16e9 calls in 45 s). The handle slot reads 0, so every call returns 0xe401 (InvalidHandle). It starts ~0.7 s in, right after apm OpenSession. NAPPLE_FREE_CORES=1 and installed system fonts do not change it. The waiting code is in main around main+0x3b53a68 (loops on apm SetPerformanceConfiguration 0x10001/0x20003, compares a field at [obj+0x104]); it waits for an applet/apm performance-mode state or event handle that never arrives. Next blocker (owner: applets/apm services): see the shared notes file `ac-303-stall-findings.md`.
LAS and SMO shaders are cheap (LAS 18 shaders, translate avg 0.1 ms, Metal compile avg 0.2 ms, max 1 ms; SMO 10 shaders), so no compile-speed change was made without evidence of a slow path.

## Changed (gfx/src/mtl_shader.m only)
- Per-step timing: Maxwell->MSL translate, Metal library compile (async path and launch prewarm), pipeline state creation; counts/avg/max.
- `LOGW` "slow shader:/slow pipeline:" for any single step over 1.5 s (names the program hash, stage and code size).
- Progress line every 2 s while anything moves: `shader progress: shaders built B of Q queued (F failed, D from disk cache); launch prewarm x of y | translate n avg/max | metal compile n avg/max | pipelines n avg/max | elapsed, ETA ~s`. `NAPPLE_GFX_SHADER_LOG=0` silences it. A final `shader summary:` line is logged at shutdown.
- Window title progress not done (frontend/runtime code is not in this area; the existing "Preparing shaders x / y" boot status covers the warm-cache launch).

## Verified
make -C gfx test: ALL TESTS PASSED. LAS and SMO 45 s headless cold cache: exit 0, 0 CRASH, frames 2337 / 2229. AC 3.0.3: frames 0 (blocker above). Progress line itself only exercised by the end-of-run summary (LAS/SMO finish building in under 2 s); the periodic line was not observed firing.
