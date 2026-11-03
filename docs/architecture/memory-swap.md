# iOS memory: disk-backed swap, pressure handling and the footprint audit

Problem: iOS kills an app (jetsam) when its **`phys_footprint`** passes a per-process limit, about half of RAM by default on current iPhones. A heavy title maps about 3.2 GiB of guest memory
and keeps most of it dirty, so the app is killed after a minute or two. iOS has no swap for anonymous memory, but **dirty pages of a file mapping can be written back to the file and evicted**, and
the kernel does not charge them to the footprint the way it charges anonymous pages. The runtime can therefore back its big, rarely touched regions with an unlinked temp file ("swap").

Everything here is off by default. `NAPPLE_IOS_SWAP=1` turns the swap on (the name says iOS because that is the target; the code is the same on macOS, where it is the test bed). Memory-pressure
handling is on by default on iOS and `NAPPLE_MEMPRESSURE=1` elsewhere. Status: **verified on macOS and the iOS Simulator, never run on a physical device** (see "What needs a device").

## How the kernel accounts the two kinds of memory (measured)

`tools/swap_probe.c` maps N MiB anonymous and N MiB file-backed (`MAP_SHARED`, unlinked temp file), dirties every page, and prints `task_info(TASK_VM_INFO)` after each step. 2048 MiB each:

| Step | macOS 27 (native) footprint / internal / external | iOS Simulator (iOS 27 runtime, host kernel) footprint / internal / external |
|---|---|---|
| anonymous, dirtied | **2050** MB / 1876 / 0 (172 MB compressed) | **2058** MB / 808 / 13 (1247 MB compressed) |
| file-backed, dirtied | **2** MB / 0 / 2048 | **10** MB / 0 / 2061 |
| file-backed, after `msync` + wait | 2 MB / 0 / 2048 | 10 MB / 0 / **627** (pages already evicted by the kernel) |
| file-backed, read back | no mismatch | no mismatch (1170 MB faulted back in), data intact |

So: anonymous dirty memory is 100 % in `phys_footprint` (compressed pages included); dirty file-backed pages are **external** and add nothing to it. Under host memory pressure the Simulator run
evicted most of the file-backed pages (written to the file) and later read them back unchanged. The Simulator is a macOS process, so this is the macOS kernel's ledger; the iPhone kernel is the same
XNU ledger code (`phys_footprint` = internal + compressed + IOKit/Metal + page tables ..., never external pages), but the **device policy is unmeasured** (see "What needs a device").
Rerun the probe on the device to settle it; the procedure is at the end.

Real runtime, headless, 45 s, macOS (`memlog: footprint`, MB; guest-mapped is the committed guest memory, metal the `MTLDevice` allocation):

| Title (test set) | guest-mapped | metal | footprint, swap off | footprint, `NAPPLE_IOS_SWAP=1` | frames (off / on) |
|---|---|---|---|---|---|
| heavy title (OOMs on the phone) | 3254 | 509 | 880 to 1030 | **272** (peak 404) | 2656 / 2653 |
| 64-bit title with 4 GiB heap | 3136 | 1050 | 2442 | **975** | 2512 / 2371 (load-dependent) |
| light title (plays fine on the phone) | 3284 | 1383 | 1420 | **942** | 2667 / 2649 |

In the iOS Simulator (same heavy title, the iOS runtime build, app container `tmp/` for the swap files): `phys_footprint` 748 MB without swap, 121 MB with it.
The headless macOS runs stay at the title screen and do not show the growth that kills the phone after a minute of play; they show where the memory is: the guest heap is 55 to 80 % of the footprint and moves out with the swap,
the Metal figure (GPU-private textures, pipelines) does not.

## What is file-backed and what stays in RAM

| Region (file: `runtime/src/...`) | Size (heavy title) | Backing with `NAPPLE_IOS_SWAP=1` | Why |
|---|---|---|---|
| Guest heap, `SetHeapSize` (`memory.c`) | up to 3.25 GiB device cap (4 GiB elsewhere); ~3.2 GiB committed | **file-backed** (sparse `heap` file of the cap size; grows by `mmap(MAP_FIXED, MAP_SHARED)` of the new range, shrink punches the hole and returns the range to `PROT_NONE`) | the bulk of the footprint; most pages are cold after load |
| Alias region, `MapPhysicalMemory` (`memory.c`) | 0 to a few hundred MB | **file-backed** (sparse `alias` file; 16 KiB-aligned commit/decommit, hole punched on unmap) | same |
| Shared memory objects >= 4 MiB (`ipc.c`) | fonts, small | **file-backed** (`swap_map_anywhere`) | cold after the first read |
| Thread stacks and `MapMemory` destinations in the stack region | small | anonymous | hot, written constantly |
| Module images (`module.c`, iOS: dyld-mapped dylibs) | 100 to 300 MB | dylib text is clean file-backed already; relocated data stays anonymous | text is evictable for free; data is small |
| RomFS / data | 0 | **not mapped at all**: `fs.c` serves it with `pread` (`romfs.bin` or `romfs.map` extents) | nothing to swap |
| Metal pool buffers over the guest heap (`mtl_core.m`, `newBufferWithBytesNoCopy`) | the heap itself | follow the heap's backing | GPU reads of cold pages simply fault them in |
| Metal textures, render targets, pipelines (`mtl_tex.m`, `mtl_replay.m`) | 300 to 500 MB | GPU-private, counted in the footprint | in-use GPU textures stay resident; there is no safe eviction (they are rebuilt from guest memory only on invalidation) |
| Per-submission scratch ring chunks (`mtl_core.m`, write-combined shared) | 4 MiB chunks, <= 16 parked | RAM; **parked chunks are dropped on pressure** | per-frame hot data; the parked ones are only a cache |
| Shader / pipeline caches on disk (`cache_trim.c`) | capped at 1024 MB | on disk already (`NAPPLE_GFX_CACHE_MAX_MB`) | not memory |

Hot per-frame data therefore never sits behind a file mapping: it lives in the stack region, the scratch ring and GPU-private textures. The guest heap can hold hot pages too (the game's own working
set); for those the file mapping is harmless, because a page that is touched often stays resident (it is dirty-then-clean-then-dirty, the kernel only evicts what is not used) and a fault on an evicted
page is a read from the page cache or the SSD.

## The swap region allocator (`runtime/src/swap.[ch]`)

* `swap_file_open(f, name, size)` creates `napple-swap-<name>.XXXXXX` in `NAPPLE_SWAP_DIR`, else `$TMPDIR`, else `/tmp` (on iOS `$TMPDIR` is the app container's `tmp/`), `unlink`s it **at once**, and
  `ftruncate`s it to the size (sparse: no disk is used until pages are dirtied). Exit, crash, jetsam or force-quit all drop the last reference, so the space cannot leak.
* `swap_commit(f, region, off, len)` = `mmap(region+off, len, RW, MAP_SHARED|MAP_FIXED, fd, off)` over the `PROT_NONE` reservation `memory_init` already made. Address == guest address, so nothing else in the
  runtime changes: signal-based write tracking (`mprotect`), `mach_vm_remap` code aliases and Metal no-copy wrapping work on a file mapping like on anonymous memory.
* `swap_decommit` maps anonymous `PROT_NONE` back over the range and `fcntl(F_PUNCHHOLE)`s the file range, so freed memory returns its disk blocks and reads back as zero when committed again.
* The file size is the region cap, sparse; real disk use is the pages written back, at most the dirty working set (about 3.2 GiB for the heavy title). The check `statvfs` is not done: a sparse file
  never reserves blocks, and an out-of-space write-back makes the kernel keep the page dirty (the footprint is unaffected). **Keep 4 GiB of free storage** on the device for long runs.
* Failure to create the file (read-only directory, no space) logs `swap: ...` and the region falls back to the old anonymous path: swap can only help.

## Eviction policy

Dirty file pages are written back by the kernel on its own schedule, but "can be evicted" must be true *before* the system is in trouble. So the runtime keeps the pages clean and tells the kernel which ones to
reclaim first:

| Trigger (`runtime/src/mempressure.m`) | Level | Action |
|---|---|---|
| every 5 s, swap on | 0 idle | `msync(MS_ASYNC)` of a rotating 64 MiB slice of each swap file: dirty pages become clean, evictable at no cost |
| `os_proc_available_memory()` < `NAPPLE_MEM_WARN_MB` (800), or kernel memory pressure *warn* | 1 | `msync(MS_SYNC \| MS_DEACTIVATE)` of a 256 MiB slice per call (cleans, then moves the pages to the inactive queue so the kernel reclaims them first); `gfx_mtl_trim(1)` frees the parked scratch ring chunks |
| available < `NAPPLE_MEM_CRIT_MB` (400), kernel *critical*, or `applicationDidReceiveMemoryWarning` | 2 | the same over the **whole** swap files; chunks freed |

The poll runs once a second. `MS_DEACTIVATE` is a hint: a page the game touches again is simply faulted back. Pages are chosen by address (a rotating cursor), not by recency, because the kernel owns the
recency information; the deactivation makes it consult that information first. A smarter policy (per-region hot/cold tracking from the write-fault tracker) is possible later and not needed to meet the
measured numbers.

On a Mac, `NAPPLE_MEM_BUDGET_MB=<n>` pretends a limit (available = n - `phys_footprint`), which exercises the whole path without a device. Example: with a 1100 MB budget the heavy title crossed the warn line
at startup, level-1 trims fired, and `phys_footprint` settled at 272 MB.

## Entitlements (what the build can ask for)

| Entitlement | Free Apple ID | Paid developer account | Where |
|---|---|---|---|
| `com.apple.developer.kernel.increased-memory-limit` | not available | capability on the App ID; Studio asks for it | `IOSEntitlementPlan` (`studio/Sources/NappleStudioCore/SigningAccount.swift`), stub project via `Provisioner` |
| `com.apple.developer.kernel.extended-virtual-addressing` | not available | capability on the App ID; Studio asks for it (added by this work) | same |
| `com.apple.developer.kernel.increased-debuggable-memory-limit` | not available | debug builds only | same |

The CLI (`napple build --target ios`) signs with **exactly the entitlements of the provisioning profile** (an entitlement the profile does not carry makes iOS refuse to launch the app), and now prints a note
when the profile lacks the increased-memory or the extended-virtual-addressing capability. Free-account builds are unchanged. The swap is the part that works **without** a paid account, which is why it exists:
it is the only lever for a free Apple ID. With the entitlements the limit is higher *and* the swap still lowers the footprint, so they combine.

## Metal eviction under pressure

Implemented: parked scratch ring chunks are released on any pressure level (`gfx_mtl_trim`, thread-safe, rebuilt on demand). Not implemented, with reasons: texture eviction (textures are
GPU-private and rebuilt only from guest writes; evicting an in-use one stalls a frame and can corrupt a render target), pipeline-state eviction (recompiling a pipeline costs a visible hitch;
the on-disk binary archive makes it cheap but the memory is small), and `MTLPurgeableState` on textures (the cached ones are the dummy textures, a few KB). The texture working set is
the ~300 to 500 MB `metal` figure in the log; reducing it means lowering the render scale or using handheld mode (the iOS default).

## Reading the log

`memlog` (on by default on iOS, `NAPPLE_MEMLOG=1` elsewhere, `NAPPLE_MEMLOG_SECS`, default 20) prints:

```
memlog: footprint 272 MB (peak 407 MB) guest-mapped 3254 MB metal 509 MB resident 690 MB
memlog: swap: backed 3200 MB resident 834 MB written-back 640 MB deactivated 256 MB trims 11
memlog: system still allows 2310 MB before the limit          (iOS only: os_proc_available_memory)
mempressure: level 1 (available memory low): trimming
```

`footprint` is what jetsam judges; `resident` includes the file-backed pages; `swap resident` is the file-backed part in RAM right now (mincore); `still allows` is the number that must stay positive.

## Device test procedure (needs a paid account for step 3)

1. Install the iOS build as usual ([ios.md](ios.md)). Launch it **without** the debugger (`devicectl device process launch`, or tap the icon): an attached debugger changes the limit and the fault handling.
2. Baseline: no variable. Play the heavy title for 5 minutes with Xcode's *Debug navigator > Memory* gauge or `memlog` lines (Console.app, device > process `napple-runtime`). Note `footprint` and `system still allows`.
   Expected today: `footprint` climbs to the limit and the app is killed (the `Memory` line of the crash log says `per-process-limit`).
3. With the entitlements: rebuild signed with a paid profile; `still allows` should start roughly 2x higher. (Skip with a free Apple ID.)
4. With swap: set `NAPPLE_IOS_SWAP=1` in the scheme (Xcode: Edit Scheme > Run > Arguments > Environment) or `SIMCTL_CHILD_NAPPLE_IOS_SWAP=1` for the Simulator; `devicectl device process launch --environment-variables '{"NAPPLE_IOS_SWAP":"1"}'`.
   Look for `swap: heap: 3328 MB sparse backing file`. Expected: `footprint` stays far below the unswapped run (the probe predicts the heap leaves the footprint), `swap resident` plateaus, `written-back` grows by 64 MB every 5 s.
5. Soak 30 minutes. Pass = no `Jetsam`/`per-process-limit` crash log under Settings > Privacy > Analytics, `still allows` never below ~300 MB, fps unchanged within noise. Watch for stutter: it means pages evicted and re-read
   (look at `deactivated` growing steadily while the game is idle on the same screens; try `NAPPLE_MEM_WARN_MB=300 NAPPLE_MEM_CRIT_MB=150` to trim less).
6. Settle the accounting question: build `tools/swap_probe.c` for the device (copy it into a throwaway app's `main`) and compare with the Simulator table above. If the device's `footprint` stays small while `external` grows, the design holds; if it
   grows, report it: the fallback is `NAPPLE_HEAP_CAP_MB` smaller plus the entitlements.

## What needs a device (unmeasured here)

* Whether the iOS memory-status daemon (`memorystatus`) charges mapped-file dirty pages (it uses the same `phys_footprint` ledger, so it should not; the probe on the device confirms).
* Write-back rate and SSD wear over 30 minutes: the dirty working set that changes is rewritten; budget a few GB written per hour for a busy title.
* Page-fault latency for GPU reads of evicted pages (Metal wraps the heap without copy; a GPU access to a non-resident page is handled by the kernel, but the cost under thermal load is unknown).
* Whether `os_proc_available_memory` crosses the thresholds before jetsam acts on a fast allocation burst (the poll is 1 s; the kernel pressure source covers bursts).

## Files

`runtime/src/swap.[ch]` (allocator, trim, stats), `runtime/src/memory.c` (heap, alias), `runtime/src/ipc.c` (shared memory), `runtime/src/mempressure.m` (signals and policy), `runtime/src/memlog.m` (log),
`gfx/src/mtl_core.m` (`gfx_mtl_trim`), `frontend/src/ios/frontend_ios.m` (memory warning), `tools/swap_probe.c`, `runtime/tests/host/test_swap.c`, `studio/Sources/NappleStudioCore/SigningAccount.swift`,
`crates/napple-build/src/ios.rs`.
