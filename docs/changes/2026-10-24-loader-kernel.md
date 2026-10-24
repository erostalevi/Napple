# Loader / kernel round: guest IPC, code memory, NRO loading, host1x channels, save data management

Status: done for this round, **untested on macOS**. The work was done on a Linux x86_64 box, where `runtime/` does not build: it needs
mach, CommonCrypto, Cocoa/Metal and arm64 assembly. Everything that could be made portable was, and is unit tested there through the new
`make -C runtime test-host`. The macOS-only glue was only syntax-checked against stand-in headers. Details per item are in
docs/runtime-services.md ("Kernel, loader, nvdrv and fs additions").

## What changed
- **Kernel**
  - Guest-to-guest sessions, light sessions and ports (`ksession.c`, message copy `kmsg.c`): svcs 0x20, 0x40-0x44, 0x70-0x72, plus routing
    in ipc.c's SendSyncRequest / ConnectToNamedPort.
  - SynchronizePreemptionState with TLR pinning at the svc-entry checkpoint.
  - Real SetThreadActivity pause (parking at svc boundaries), GetThreadContext3, GetLastThreadInfo, and the debug svcs 0x2E / 0x3C / 0x3D.
  - Code memory 0x4B / 0x4C (`kcode.c` + mach_vm_remap aliases in `memory.c`), and CreateSharedMemory 0x50.
  - threads.c now shares its thread struct and wait primitives through `kthread.h`. WaitSynchronization's loop became
    `kthread_wait_handles` (also used by ReplyAndReceive).
- **Loader**
  - `ldr:ro` / `ro:1` (`ro.c`, NRO/NRR checks in `nro.c`, portable SHA-256 in `sha256.c`).
  - Run-time modules in `module.c`: `module_load_nro` / `module_unload`, 128 module slots, skipping of unloaded ones.
  - The gate index (`dispatch.c`) is rebuilt and published atomically on load/unload.
  - module.c no longer needs CommonCrypto or libkern (own SHA-256, `__builtin___clear_cache`).
  - `napple build` rewrites RomFS NROs (`crates/napple-build/src/nro.rs`, pipeline step, manifest `nro`).
  - `build_tables` accepts an NRO header.
  - The rewriter's scan treats an NRO header inside text as data.
- **nvdrv**: per-fd host1x channel state for nvdec/vic/nvjpg/msenc, with syncpoint allocation, validated SUBMIT, MAP/UNMAP_CMD_BUFFER device
  addresses and error returns.
- **fs**
  - Save data create/delete/extend, extra data (read, write, masked, by id and by attribute) and info readers.
  - Cache storage size and delete, and size queries of sized saves.
  - Temporary storage emptied at first mount, and a truly read-only OpenReadOnlySaveDataFileSystem.
  - Fixes from the journal review (below).
- **Portability for tests**
  - ipc.c only uses mach_vm_remap on Apple (elsewhere the existing copy path).
  - threads.c has a POSIX timed wait and no QoS outside Apple.
  - fs.c uses the recursive copy instead of clonefile, and st_ctime instead of st_birthtime, outside Apple.

## Review of the transactional save code
The snapshot/swap/recover scheme itself holds up: every crash point leaves the old or the new snapshot, and recovery finishes an
interrupted swap. Issues found and fixed:
1. `OpenReadOnlySaveDataFileSystem` mounted writable and created missing saves.
2. `Commit` cleared the dirty flag after the snapshot and outside the lock. A write racing with the commit vanished from the
   uncommitted-changes report, and a failed commit could still leave dirty cleared.
3. A failed rename of the new snapshot left `.committing` behind (only cleaned at the next commit or start).
4. `CleanDirectoryRecursively` changed the save without marking it dirty.
5. More than 16 transactional roots silently fell back to non-transactional; that is now logged.
6. A save deleted and recreated in one process kept a journal marked ready, so recovery never ran for the new root.

Not changed: the dirty flag is only a report, and commits always snapshot the live directory. `sync()` after a commit is global, not a
per-directory fsync. Commits of a file a game is still writing are torn exactly as they would be anywhere.

## Verified (Linux x86_64, gcc 13 and clang 18; `make -C runtime test-host`, all passing)
- `test_kernel_ipc` (84 checks), using real host threads adopted as guest threads and calling the svc handlers:
  - HIPC copy: layout, pid, pass-through, X data into indexed and single receive lists, broken receive list, too large.
  - Sessions: receive, reply, reply-only TimedOut, SessionClosed on either side, through ipc.c's real SendSyncRequest.
  - Light sessions, and ports including max sessions, port closed, and named ports registered, connected and removed.
  - CreateCodeMemory / ControlCodeMemory checks, with a recording fake for the host alias.
  - Pause / resume / GetThreadContext3 (registers, fp, lr, guest sp; parked while signaled; Busy, InvalidState).
  - GetLastThreadInfo answers; SynchronizePreemptionState clears the TLR flag.
  - CreateSharedMemory, MapSharedMemory (copy path), UnmapSharedMemory.
  - Clean under ThreadSanitizer (3 runs) and ASan+UBSan (20 runs).
- `test_loader` (40):
  - A static module loaded through `modules_load` from a synthesized data dir, then a synthesized rewritten NRO mapped through the
    `ldr:ro` handlers: RELATIVE, ABS64 to the static module with addend, GLOB_DAT to a host override, weak null, NRO exports visible,
    key page filled.
  - The NRO's svc and cntfrq gates dispatched, unload, and slot reuse on reload.
  - NRR/NRO validation errors, NotAuthorized, AlreadyLoaded, missing image (lenient), ro:1 kind check, session close unloads.
- `test_nvdrv` (29): all channel ioctls above through the service's IPC handlers, including error returns and that nvhost-ctrl sees the
  submitted syncpoint value.
- `test_fs_savedata` (33): every new fs command above, sizes, temporary storage, read-only mounts, delete resets the journal.
- `test_fs_journal` (16): the existing journal test, now also built for Linux; it passes using the copy fallback.
- Rust:
  - `cargo test --workspace` passes, including the new `nro::tests` (header parsing and rejection; rewrite of a synthesized NRO into the
    runtime layout with module id in the tables and an untouched header) and `scan::tests::nro_header_is_data`.
  - The NTB1 golden test is unchanged, so the bytes for NSOs are the same.

## Untested
- **Anything on macOS**: building and linking the runtime, the existing `make -C runtime test`, and running a game.
  - memory.c (code memory aliases), threads.c (Apple timed wait / QoS), ipc.c (remap path) and module.c were only checked with
    `gcc -fsyntax-only -D__APPLE__` and stand-in mach headers.
  - mach_vm_remap of code memory and making that alias executable under the hardened runtime are untested.
- **The real gate**: the GetThreadContext3 / GetLastThreadInfo sp and q-register offsets come from reading gate.S; they were not run
  through it.
- **Real games**: no title known to the project uses guest sessions, light IPC, code memory, ro or the nvdec channels at this level.
  - Whether the SDK's nn::ro user-mode relocation pass keeps or overrides the runtime's bindings (host overrides) is unknown.
  - Small NROs whose text end and data start share a 16 KiB page will fail to seal, as static modules would.
- **`napple build` against a real RomFS with NROs**: only the unit test with a synthesized NRO ran.
- **Protocol constants taken from public descriptions, not hardware**:
  - the light-session reply flag (bit 31);
  - nvhost channel SUBMIT layout (0x14-byte increments, u32 fences);
  - fsp-srv argument layouts for commands 57/59/66/69-71; the (space, attribute) order is detected at run time.
- **macOS-specific fs paths**: the APFS clonefile path of the journal (Linux used the copy fallback), and `st_birthtime`.

## Files
- runtime/src: new kthread.h, ksession.c, kmsg.c/.h, kcode.c, ro.c, nro.c/.h, sha256.c/.h; changed threads.c, kernel.c, ipc.c, memory.c,
  module.c, dispatch.c, napple.h, main.c (registers ldr:ro).
- runtime/src/services: nvdrv.c, fs.c.
- runtime/tests/host: new; runtime/Makefile: `test-host`.
- crates/napple-build: nro.rs (new), pipeline.rs, tables.rs, lib.rs; crates/napple-rewrite: scan.rs.
- docs: runtime-services.md (appended), runtime-modules.md (appended), this note.

## Branch
The request named branch `worktree-loader-kernel`, which does not exist in this checkout or on the remote. The session's assigned branch is
`claude/clever-galileo-sq7426`. The commit is on that branch, and a local branch `worktree-loader-kernel` points at the same commit.
Nothing is pushed.
