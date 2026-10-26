# ac-boot-spin: Animal Crossing 3.0.3 boot-thread spin fixed, ngc:u added, honest "no frame yet" text (2026-10-24)

## Root cause (verified)
The 25M/s `WaitSynchronization(handle 0, timeout 0) -> 0xe401` loop was the SDK's nn::oe layer polling a system event it never received. Chain: game (main+0x3b53a68) -> sdk+0x407d60 (lazy-initialises a static event) -> the init calls an `IApplicationFunctions` command and attaches the returned `{copy handle, managed}` to the event -> the poll `TryWait` reads the handle (0) and calls svc 0x18.
`IApplicationFunctions` cmd **210** was implemented as `CreateMovieMaker` returning an IMovieMaker *object* (commit ff55c90, a guess). The real reply of cmd 210 carries a **copy handle to an event**. Our reply had no copy handle, so the guest kept handle 0 and the boot thread spun forever. It was not apm (apm OpenSession/SetPerformanceConfiguration are fine; thread 0x56 "never started" was a side effect) and not shaders.
Fix: cmd 210 now returns a copy handle to a never-signalled event (`services/applet.c`). Proof: `NAPPLE_TRACE_IPC=all` shows the spin start right after `ICommonStateGetter::GetOperationMode` and, with the fix, boot continues (audio renderer, timezone, `set`, ...).

## Changed
- `runtime/src/services/applet.c`: cmd 210 -> event copy handle (name `GetSystemEvent210`; the true command name is unknown, the handle semantics are verified against the guest). IMovieMaker removed.
- `runtime/src/services/ngc.c` (new) + registration: `ngc:u` (profanity filter; GetContentVersion/Check/Mask/Reload; nothing is ever flagged). AC aborted with 0xa15 right after the timezone calls without it ("service ngc:u is not implemented").
- `runtime/src/app.m`: the static "First launch: shaders are compiled..." text now reads "Starting the game. First launch: shaders are compiled as the game draws them..."; after 8 s with `napple_gfx_frame_count() == 0`, no blocking compile and no pipeline, it says "Still starting: the game has not drawn its first frame after N s (no shaders needed yet)" and goes back to "Starting" once a frame exists. (Compiles; not seen in a window, headless has no frontend.)
- Tests: `tests/test_svc_ngc.c` (new), `test_svc_applet.c` gets a cmd-210 check.

## Result (AC 3.0.3, build ff1d1c05670db602, headless, cold)
Boot now runs ~30 game threads and reaches the main loop, but **gfx frames are still 0** (next blocker below).

## Next blocker (named, not fixed)
After ~0.96 s no more IPC is issued. Observed at 10-45 s:
- main thread (id 0x51) spins in user mode inside `nn::hid::GetNpadStates` called from its game loop (~188 svcs in 30 s), never reaches a draw;
- two worker threads (0x71/0x72, a message/job thread pair at main+0x3e4xxxx) ping-pong one mutex: ~15M ArbitrateLock/ArbitrateUnlock each in 30 s (also with NAPPLE_FREE_CORES=1), i.e. busy polling a condition nobody sets;
- every other game thread (sead-style thread loops, main+0x37e336c) sleeps on its message queue (WaitProcessWideKeyAtomic).
Something the main loop waits for (a load/job completion or a state flag) never completes. Suspects: HID shared memory state for the Npad AC reads (hid ring is consistent: sampling 0x47a..0x48a, tail 6, count 17, so not a torn read), a file/storage completion (no IStorage::Read after 0.7 s), or an event never signalled (thread 0x52, prio 28, waits forever on the `IApplicationFunctions` cmd 130 event, which is the GPU-error event and expected to stay idle). Next step: find what the 0x71/0x72 loop polls (object at main+0x3e43c.. loop, queue pop at main+0x3e42eb0) and who should fill it; `lldb -p <pid>` samples map to modules via the `loader: mapped` lines; `tools/addr2sym.py <data-dir> main+0x...` names functions.

## Verified
- `make -C runtime test test-services`: all pass (rc 0, 34 service tests + test_ipc etc.).
- LAS and SMO 45 s headless: exit 0, 0 CRASH/FAIL, frames 2304 / 2210.
- AC 3.0.3 45 s headless: exit 0, 0 CRASH/FAIL, frames 0 (blocker above).
