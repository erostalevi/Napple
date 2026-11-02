---
name: ios-memory
batch: 2026-11-02-r8
branch: claude/ios-memory-2026-11-02-r8
base: orchestrate/2026-11-02-r8
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_9f9355cb-2ac1-4f06-89c8-bee5ccc3ed38 (fresh)
launched_at: 2026-11-02
---

# ios-memory: Super Mario Odyssey runs out of memory on iPhone after about a minute

## Goal
User report (2026-11-02, played by a person on an iPhone 16 Pro Max): LAS plays perfectly at 60 fps at 1x render; **SMO runs at 60 fps at 1x, then is
killed for memory after about a minute**. Make SMO stay within the iOS app memory budget so it plays for 30+ minutes on that device.

## Work
1. **Entitlements**: the iOS build requests `com.apple.developer.kernel.increased-memory-limit` and `com.apple.developer.kernel.extended-virtual-addressing`
   (wherever the iOS app's entitlements are produced: `crates/napple-build` iOS packaging and/or `tools/build_ios.sh`); document that the provisioning
   profile must include them (free Apple IDs may not get them: detect and report at build time, never fail the build silently).
2. **Footprint audit** with the iOS Simulator and macOS `footprint`/`vmmap`/Instruments-style counters on SMO: guest heap/address-space mapping (are
   reserved-but-untouched pages being committed?), Metal texture/buffer caches and staging buffers, shader/pipeline caches, RomFS map/file caches, audio
   buffers, recorder command memory. Produce a table: component -> resident MB over 5 min of SMO, before/after.
3. **Fixes**: bounded caches with LRU and size budgets derived from `os_proc_available_memory()` on iOS; `MTLResourcePurgeableState`/heaps for caches;
   release on `didReceiveMemoryWarning` / `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE` (evict caches, trim staging, drop decoded RomFS blocks); avoid committing
   guest memory the title never touches (`MAP_NORESERVE`-style / `madvise(MADV_FREE)` where pages are freed by the guest); log the footprint every 10 s
   under `NAPPLE_MEM_LOG=1`.
4. macOS must not regress (same caches, larger budgets).

## Owned paths
`frontend/src/ios/` (memory-pressure handling; coordinate: cloud-saves-client may add a small Keychain/sync hook there — keep edits in separate
functions/files and merge cleanly), `tools/build_ios.sh`, the iOS entitlements/packaging code in `crates/napple-build/` (ios packaging only),
`gfx/src/mtl_tex.m`, `gfx/src/mtl_replay.m` cache/staging paths, `gfx/src/mtl_shader.m` cache sizing, `gfx/src/mtl_priv.h` (append),
`runtime/src/memory.c` (guest mapping policy), `runtime/src/services/romfs_map.*` caches, new `docs/architecture/ios-memory.md`, own change note.

## Do not touch
`runtime/src/net/`, `runtime/src/cloud/`, `crates/napple-account|online|room|cloud|cli`, `studio/`, `infra/`, `orchestrations/`.

## Acceptance
1. Footprint table (before/after) for SMO in the iOS Simulator over 5+ minutes of a scripted run, plus macOS; steady-state footprint below the iPhone
   budget with the increased-memory-limit entitlement (state the number you target and why).
2. Simulated memory pressure (simulator "Simulate Memory Warning" / `memory_pressure` tool on macOS) frees caches without visual corruption.
3. iOS build succeeds with the entitlements (device build at least compiles/signs ad hoc; report what the user's provisioning must include).
4. macOS four-title boots no regression; `make -C gfx test`, runtime tests green.
5. Hand-off says plainly that the final check is the user playing SMO 30 min on the iPhone 16 Pro Max.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data read-only, locks, local-only, never ask, never push). Simulator runs count toward the run lock.

## Result
