---
branch: claude/ios-memory-swap-31951e
orchestration: none
base: 7f613e87236e39919664138f42c5152ece1e81a6
head: f50087570866c0d8c66a2673a52cbf7ca585dac4
env: mac
date: 2026-11-03
areas: [runtime/memory, gfx/mtl_core, frontend/ios, studio/signing, crates/napple-build/ios, docs]
status: integrated
---

# claude/ios-memory-swap-31951e: disk-backed swap, memory-pressure handling, entitlements and footprint audit for iOS

## Summary
`NAPPLE_IOS_SWAP=1` (default off) backs the guest heap, the alias region and shared memory >= 4 MiB with unlinked sparse temp files mapped `MAP_SHARED`. Measured: dirty file-backed pages are `external` and
not part of `phys_footprint` (the jetsam figure): 2 GiB dirtied = 2 MB footprint vs 2050 MB anonymous, on macOS and in the iOS Simulator; evicted pages read back intact. On macOS the heavy test title's
footprint went 1030 -> 272 MB, the 4 GiB-heap title 2442 -> 975 MB, another 1420 -> 942 MB (GPU-dominated). `mempressure.m` adds trimming (swap write-back/deactivate, parked Metal scratch chunks) from the kernel pressure
source, an `os_proc_available_memory` poll and the UIKit memory warning. Studio now also asks for `extended-virtual-addressing` (paid accounts); the CLI notes when a profile lacks the memory entitlements.
Design note `docs/architecture/memory-swap.md` has the audit, policy and the device test procedure. NOT done: any physical-device verification (the 30-minute target is a device test); Metal texture eviction (judged unsafe, reasons in the note).

## Files
New: `runtime/src/swap.[ch]`, `runtime/src/mempressure.m`, `runtime/tests/host/test_swap.c`, `tools/swap_probe.c`, `docs/architecture/memory-swap.md`, `docs/archive/changes/2026-11-03-ios-memory-swap.md`.
Changed: `runtime/src/memory.c`, `ipc.c`, `main.c`, `memlog.m`, `runtime/Makefile` (new host test; `src/swap.c` added to `HOST_KERNEL`), `gfx/src/mtl_core.m` + `gfx_mtl.h` (`gfx_mtl_trim`), `frontend/src/ios/frontend_ios.m`,
`studio/Sources/NappleStudioCore/SigningAccount.swift` (+ `RedoTests.swift` expectations), `crates/napple-build/src/ios.rs` (note only), `docs/index.md`, `docs/architecture/ios.md`, `docs/reference/environment-variables.md` (append-only rows).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Probe: anon vs file-backed accounting, macOS | verified | `cc tools/swap_probe.c && ./a.out 2048`: footprint 2050 vs 2 MB |
| Probe in the iOS Simulator | verified | `simctl spawn`: 2058 vs 10 MB; file pages evicted 2061 -> 627 MB resident, data intact |
| Swap in the real runtime, macOS (SMO headless 45-60 s) | verified | footprint 880-1030 -> 272 MB; frames 2656 vs 2653; iOS Simulator app 748 -> 118 MB |
| Swap on/off boots of MK8D, LAS | verified | MK8D 2442 -> 975 MB (frames 2512 vs 2371, load-dependent), LAS 1420 -> 942 MB (2667 vs 2649) |
| ACNH boot | untested | the shared title data was deleted mid-session by something else; the swap-off path is unchanged code (default off) |
| Pressure path (`NAPPLE_MEM_BUDGET_MB`) | verified | LAS: level 1 then level 2 trims, rate-limited to one per 5 s, parked chunks released |
| `make -C runtime GFX=napple test test-services test-ipc-reference` | tested | rc=0 |
| `make -C runtime test-host` (incl. new `test_swap`) | tested | all ok |
| `cargo test --workspace` | tested | 555 passed, 0 failed |
| Studio `MemoryEntitlementTests` | tested | `swift test --filter MemoryEntitlementTests`: 5 passed |
| iOS Simulator runtime build + run of the swap | verified | `SDK=iphonesimulator tools/build_ios.sh`, SMO ios-sim app launched with `SIMCTL_CHILD_NAPPLE_IOS_SWAP=1`: footprint 748 -> 118 MB, `swap:` lines present |
| Anything on a physical iPhone | untested | cannot be done from here |

## Unbuilt / untested list (the integrator checks these first)
* `frontend/src/ios/frontend_ios.m` memory-warning hook: compiled for the Simulator, never triggered (the Simulator sends no memory warning unless Debug > Simulate Memory Warning is used).
* iOS device paths of `mempressure.m` (`os_proc_available_memory` on a device): compiled only.

## Findings
* RomFS is never mapped (`fs.c` uses `pread`), so it needs no swap; the whole footprint is guest heap + Metal.
* `tools/build_ios_deps.sh` output in `build-ios-deps/prefix-sim` has a libvpx built with `--disable-vp8`, but `runtime/src/movie.c` now references `vpx_codec_vp8_dx`: the Simulator runtime does not link until the deps are rebuilt
  (`SDK=iphonesimulator VPX=1 tools/build_ios_deps.sh`, check the configure flags in that script still disable VP8). I linked against a scratch copy with a one-line stub; nothing of that is in the tree.
* `os_proc_available_memory()` returns 0 in the Simulator; `mempressure.m` treats the Simulator as "unknown" (my first version saw it as critical pressure).
* Shared state was removed while I worked (`/Users/neto/NappleWork`, the main checkout's `target/`, `~/.cargo/bin`); title data for the runs was rebuilt under `$TMPDIR`. Not mine; noting for whoever cleans.
* Possible SIGTERM of one headless run (rc 143) at ~30 s while other sessions were active; not reproduced.

## How to integrate
* No ordering constraints. Conflict hotspots: `runtime/src/memory.c` (heap/alias commit), `runtime/Makefile` (append-only test rule, `HOST_KERNEL`), `docs/reference/environment-variables.md` (rows added after `NAPPLE_ALIAS_GIB`).
* Build: `make -C gfx libnapple_gfx.a build/obj/adapter/gfx_napple.o && make -C frontend && make -C runtime GFX=napple` (a stale `gfx/libnapple_gfx.a` produced a missing `_gfx_mtl_trim` link error here until rebuilt).
* All new C is portable POSIX plus Darwin `msync` flags and `F_PUNCHHOLE` (both `#ifdef`-guarded in `swap.c`).

## How to test
1. `make -C runtime GFX=napple test test-services test-ipc-reference test-host`, `cargo test --workspace`, `cd studio && swift test --filter MemoryEntitlementTests`: all pass.
2. `cc -O2 tools/swap_probe.c -o /tmp/p && /tmp/p 2048`: `file dirtied` line has footprint ~2 MB, `anon dirtied` ~2050 MB.
3. Headless title with and without `NAPPLE_IOS_SWAP=1 NAPPLE_MEMLOG=1 NAPPLE_MEMLOG_SECS=5`: compare `memlog: footprint`; `memlog: swap:` lines appear only with the swap on. Add `NAPPLE_MEMPRESSURE=1 NAPPLE_MEM_BUDGET_MB=900` to see `mempressure: level N` lines.
4. Regression guard: swap off (default) must behave exactly as before (the only unconditional change is one `swap_enabled()` check in `SetHeapSize` / alias map / shared memory).
5. Device: follow the procedure at the end of `docs/architecture/memory-swap.md` (needs the user).

## Fix policy hints
Safe to fix directly: include/link-list typos. Needs a decision: the default-on question once the device run proves the accounting; the 4 MiB shared-memory threshold; the 800/400 MB trim thresholds.

## Needs from other areas
Rebuild the Simulator deps libvpx with VP8 (tools/build_ios_deps.sh owner). A device run by the user.

## Integration result
- Date 2026-11-02. Outcome: Clean (cherry-picked onto the restructured main; the branch base predates the repo restructure, so a merge would have reintroduced removed files). Landed as the merge of integ/ios-memory-swap.
- Fixes: none to code. Docs ported onto main's structure: change note wording (no workflow vocabulary), CHANGELOG Unreleased entry; index rows applied cleanly.
- Native ports: none needed (swap.c is POSIX plus guarded Darwin flags).
- Tests on the merged tree: cargo test --workspace 555 passed; runtime test / test-services / test-ipc-reference / test-host (incl. test_swap) rc=0; frontend test ok (online social 28 checks); Studio swift test 66 tests passed (MemoryEntitlementTests 5); gen_support --check OK, test_gen_support 12 OK; check_docs: same 44 pre-existing private-archive orphans as main, none new.
- Skipped: four-title boots and swap on/off boots (no converted titles on this Mac), iOS Simulator run, any device run. Still unverified: memory warning hook, device paths of mempressure.m, ACNH boot.
- Correction: runtime make needs cargo on PATH (/opt/homebrew/opt/rustup/bin).
