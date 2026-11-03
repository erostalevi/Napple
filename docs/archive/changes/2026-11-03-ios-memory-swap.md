# iOS memory: disk-backed swap, pressure handling, entitlements, footprint audit

Goal: stop jetsam kills of heavy titles on iPhone. Design, measurements and the device procedure: [architecture/memory-swap.md](../../architecture/memory-swap.md).

## What changed
* `runtime/src/swap.[ch]`: swap region allocator (unlinked sparse temp files mapped `MAP_SHARED` over the reserved guest range, `F_PUNCHHOLE` on release, write-back and deactivate passes, `mincore` residency). `NAPPLE_IOS_SWAP=1`, default off.
* `runtime/src/memory.c`: guest heap (`SetHeapSize`) and alias region (`MapPhysicalMemory`) commit through the swap files when it is on; `ipc.c`: shared memory >= 4 MiB.
* `runtime/src/mempressure.m`: kernel memory-pressure source, 1 s available-memory poll (`os_proc_available_memory`, or `NAPPLE_MEM_BUDGET_MB` as a pretend limit on a Mac), UIKit memory warning hook; on by default on iOS, `NAPPLE_MEMPRESSURE=1` elsewhere.
* `gfx/src/mtl_core.m`: `gfx_mtl_trim` frees the parked scratch ring chunks. `runtime/src/memlog.m`: swap and pressure lines.
* Studio asks for `extended-virtual-addressing` next to `increased-memory-limit` for paid accounts; the CLI notes when a profile lacks either. Free accounts unchanged.
* `tools/swap_probe.c`: anonymous vs file-backed footprint probe for macOS, the Simulator and a device.
* Audit: RomFS is `pread`-served (never mapped); the guest heap is the footprint.

## Verified / not
* Verified on macOS and the iOS Simulator: dirty file-backed pages are `external` and do not count in `phys_footprint` (2 GiB: 2 MB vs 2050 MB); evicted pages read back intact.
* Verified on macOS: real titles boot with the swap off (unchanged) and on (footprint down, frame counts equal); `make -C runtime GFX=napple test test-services test-ipc-reference`, `test-host` (new `test_swap`), `cargo test --workspace` (555 passed), Studio `MemoryEntitlementTests`.
* Built for the iOS Simulator (runtime links; needed a stub for a VP8 symbol the prebuilt Simulator libvpx lacks, the prebuilt deps must be rebuilt with VP8, see `tools/build_ios_deps.sh`).
* **Not verified: anything on a physical iPhone** (device accounting, write-back rate, GPU fault latency). The 30-minute target is a device test; the procedure is in the design note.
