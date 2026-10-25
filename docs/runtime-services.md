# Runtime services and kernel surface

The runtime (`runtime/`) hosts a rewritten game in one process: the guest's AArch64 code runs natively, `svc` and counter
reads go through `runtime_gate` (rewritten ahead of time), and the services below answer the IPC the SDK issues. Services are
implemented at IPC level (HIPC/CMIF, with domains), so SDK-version differences do not matter, except for the few host
overrides of guest imports (NVN, `nv::` graphics memory) listed at the end.

Policy: a service or command that is not implemented stops the runtime with its name and id, records it in the compat
summary (`missing_ipc`) and the trace. A stub (marked `STUB` in the command tables) answers a documented safe default.
`NAPPLE_IPC_LENIENT=1` returns an error result instead of stopping, to enumerate more gaps in one run;
`NAPPLE_SVC_LENIENT=1` does the same for svcs. The service manager refuses services outside the NPDM access list, like the
real one (the SDK falls back, e.g. `pctl` then `pctl:s`, `pctl:r`, `pctl:a`).

## Kernel (svc)

Implemented: 0x01-0x06 memory (SetHeapSize, permissions, MapMemory as a copying stack mapping, QueryMemory), 0x07 ExitProcess,
0x08-0x10 threads, 0x11/0x12/0x17 events, 0x13/0x14 shared memory (remap, or copy when not host-page aligned), 0x15
CreateTransferMemory, 0x16 CloseHandle, 0x18/0x19 WaitSynchronization/Cancel, 0x1A-0x1D mutex and condition variable, 0x1E
GetSystemTick, 0x1F ConnectToNamedPort (`sm:` only), 0x21/0x22 SendSyncRequest, 0x24/0x25 ids, 0x26 Break (reported with guest
backtrace), 0x27 OutputDebugString, 0x29 GetInfo (ids 0-28), 0x2A/0x2B cache flush (no-op), 0x2C/0x2D MapPhysicalMemory,
0x30/0x31/0x37 resource limits (plausible constants), 0x32 SetThreadActivity (existence check only), 0x34/0x35 address
arbiter, 0x45 CreateEvent, 0x51/0x52 transfer memory map (no-op: services read guest memory directly).
Virtual cores: three application cores, a thread holds one while it runs guest code (`NAPPLE_FREE_CORES=1` disables).
Everything else is unsupported and reported by number and name.

## IPC services

| Service | Status | Notes |
|---|---|---|
| `sm:` | implemented | Initialize, GetServiceHandle; NPDM access list enforced |
| `lm` | implemented | log packets forwarded to the runtime log |
| `apm` | implemented / stub | performance configuration remembered; CPU overclock stub |
| `appletOE` | implemented / stubs | proxy with ICommonStateGetter, ISelfController, IWindowController, IAudioController, IDisplayController, ILibraryAppletCreator (storage), IApplicationFunctions; focus message queued at launch; stubs: sleep locks, screenshot permission, home button, recording, media state |
| `fsp-srv` | implemented | RomFS as IStorage over `romfs.bin`; save data and sdcard as host directories; save data of type account/device/system/cache is transactional (see "Save data"); stubs: access log, auto save data creation, free space (1 GiB) |
| `set` / `set:sys` | implemented / stubs | language list, region (USA stub), firmware 18.1.0 |
| `acc:u0` | implemented / stubs | one offline user; profile image calls stubbed |
| `aoc:u` | tested | add-on content from the `dlc` array of `manifest.json`; DLC romfs mounted through `fsp-srv` command 30 (see the aoc:u section below) |
| `pctl` | stub | unrestricted device |
| `time:u/s/a` | implemented | clock contexts in the shared memory block, time zone = host |
| `hid` | implemented / stubs | shared memory + configuration through `input/` (libnapple_input); SendVibrationValue(s) / GetActualVibrationValue / GetVibrationDeviceInfo drive CoreHaptics rumble; xpad/gesture stubs |
| `vi:u` | implemented / stubs | display, layer (android Parcel native window), vsync event thread; IHOSBinderDriver answers every IGraphicBufferProducer transaction with a dummy success (the SDK of Super Mario Odyssey creates an android::Surface eagerly; NVN never uses it) |

| `pl:u` / `pl:s` | implemented | shared fonts from `<app-support>/system/fonts` (see "Shared fonts"); zero-size fonts when absent |
| `audren:u`, `audout:u`, `hwopus` | implemented | over libnapple_audio; `GetReleasedAudioOutBuffer` zero-fills unused tags (the SDK returns the first tag without checking the count); hwopus calls `guest_write_prepare` on the PCM buffer |
| `audin:u` | implemented (silent) | over libnapple_audio (`napple_audin_*`): one device "BuiltInHeadset", OpenAudioIn(Auto), IAudioIn 0-6, 8, 9, 11-14; buffers are filled in real time with silence because no capture source is installed (no microphone access, no `NSMicrophoneUsageDescription`). `OpenAudioInProtocolSpecified` (5) and the UAC appends (IAudioIn 7, 10) are not implemented |
| `nvdrv`, `nvdrv:a/s/t` | partial | `/dev/nvmap` (create/alloc/free/param/get_id/from_id as bookkeeping on guest memory), `/dev/nvhost-ctrl` (syncpoints, events), host1x channels `nvhost-nvdec/vic/msenc/nvjpg` accept Submit and complete it instantly (syncpoints advance; nothing is decoded: video is decoded by `movie.c`, see below). Every other device/ioctl is refused and listed in `missing_ipc` as `nvdrv:ioctl <path> <cmd>` |
| `mm:u` | stub | multimedia clock requests accepted |
| `friend:u` (+v/m/s/a), `nifm:u` (+a/s), `bsd:u` (+s/a), `nsd:u` (+a), `ssl`, `prepo:u` (+a/m/s), `nfp:user`/`nfc:user` | stubs (offline) | friends: empty lists; nifm: requests never accepted, connection "disconnected"; bsd: RegisterClient/Close succeed, every network call answers {-1, ENETDOWN}; nsd: environment "lp1", names resolve to themselves; ssl: contexts/connections creatable, nothing can handshake; prepo: reports dropped; nfp: initialised, no devices |
| `csrng` | implemented | host `arc4random_buf` |

Not implemented yet (reported loudly when a game asks): `mii:u`, `ns`/`ns:am2`, `ncm`, `lbl`, `psm`, `caps:*`, `spsm`, `set:fd`, swkbd applet (library applet storage), `sfdnsres`, other nvdrv devices.

## Save data (`fsp-srv`, `runtime/src/services/fs.c`)
Horizon save data is transactional and the host mirrors that so a quit or crash in the middle of a save cannot corrupt it.
`OpenSaveDataFileSystem` maps `<save-root>/<titleid>/save/space<N>/<type>[/<user>]` (the *live* directory the game writes into). For types account,
device, system and cache a sibling `<live>.committed` holds the last committed snapshot and `<live>.napple-journal` marks the root as transactional.
- **Commit** (`IFileSystem::Commit`, `nn::fs::CommitSaveData`): `tmp := clone(live)` (APFS `clonefile`, recursive copy elsewhere), `committed -> .previous`,
  `tmp -> committed`, remove `.previous`, `sync()`. A crash at any step leaves either the old or the new snapshot.
- **First open in a process** (recovery): finish an interrupted swap (`.previous` back to `committed` when `committed` is missing), delete stale temporaries,
  then `live := clone(committed)`, built in `<live>.restoring` first so a failed restore never destroys the live directory. Uncommitted work of a previous run is
  discarded, like on the console. A directory without marker and snapshot is a pre-journal save: it becomes the first committed state (no data is lost on
  upgrade). Marker without snapshot = nothing was ever committed: empty.
- Open file descriptors keep pointing at the live directory (it is never swapped), so a game that keeps a save file open across a commit keeps working;
  writes after the commit are uncommitted until the next commit.
- The process reports, when it stops, which roots still had uncommitted changes (`fs: save ...: changes since the last commit are discarded`).
- bcat and temporary storage are plain directories. `NAPPLE_FS_TRACE=1` logs CreateFile/DeleteFile/Rename/Commit with the host path.
Both games call Commit: Link's Awakening 5x at start-up (nothing pending) and after saving; Super Mario Odyssey creates `File1.bin`/`Common.bin` and commits.
Unit tests: `make -C runtime test` (`runtime/tests/test_fs_journal.c`: commit, rollback, interrupted commit, nested directories, adoption of old saves, open fds).

## Frame pacing and game speed
All guest time derives from one host clock: `GetSystemTick` and `cntpct_el0` are `mach_absolute_time` scaled to 19.2 MHz (`runtime_guest_ticks`, factor 12/625 on
nanoseconds), `cntfrq_el0` reads 19200000, `time:u` steady clock uses the same ticks, so every guest-visible clock agrees. The only periodic source is the
vsync event of `vi:u` (`vi.c`): a thread with absolute deadlines (`next += period`, no drift; a stall longer than 8 periods resynchronises instead of bursting),
period from `gfx_vsync_period_ns()` read on every tick (60 Hz unless the frame-rate setting changes it).
Settings (`frontend/include/napple_frontend.h`, applied by `app.m: push_gfx_settings` through `gfx_set_settings`, at start and on every change; headless runs read the
same `settings.json`): `fps_cap` 0 = the game's own pacing (60/30 Hz as it asks, real time, the default), 15..999 = fixed display rate the guest is paced by
(its speed scales with it: fast forward / slow motion), 1000 = uncapped. `vsync` only controls the Metal layer's display sync; it never changes game speed.
The display refresh rate (120 Hz ProMotion) is irrelevant to the guest, so a high refresh display cannot speed a game up. Measured (M5, windowless): LAS presents ~46 frames/s
(GPU ~11 ms and CPU pacing per frame: gfx replay performance, owned by graphics); the intro movie runs 2149 frames in 36.1 s (60 Hz source), i.e. real time.

## Shared fonts (`pl:u`)
The system fonts are Nintendo data and are never shipped. Extract them once from the user's own firmware:
`napple build --dump-fonts <Firmware.zip|dir> -k <prod.keys> -o "~/Library/Application Support/Napple/system/fonts"` (system data titles
0100000000000810..0815; each .bfttf is decoded: every u32 XOR 0x06186249, then {magic 0x18029A7F, u32 big-endian size, TTF}). The service lays the
fonts out in a 0x1100000-byte shared memory as {0x18029A7F, size ^ 0x49621806, TTF} and reports the offset of the TTF itself. Type mapping
(libnx PlSharedFontType): 0 std, 1 zh-CN, 2 zh-CN extension, 3 zh-TW, 4 Korean, 5 Nintendo extension, by file name.

## Host movie decoder (`runtime/src/movie.c`)
The SDK media library (`movie::Decoder`, Android MediaCodec over NvMM/NVDEC) is replaced for VP9 by libvpx: the constructor and the 17 virtual
methods are bound by symbol name through `host_symbols_register`, so the library's own vtable slots land on the host code; other decoder types
(Vorbis audio, H.264) are forwarded to the library's original code. Mode 1 (buffer) returns NV12 in the client's Buffer; mode 2 (NVN texture, used
by Link's Awakening) converts to RGBA and writes into the game's NVNtexture through `gfx_texture_write_rgba` (block-linear swizzle, dirty tracking).
Adapted from MacNativeGames (MIT), see THIRD_PARTY_NOTICES.md. H.264 (Super Mario Odyssey's tutorial movies) is not implemented.

## Per-game data (`game.json` in the data directory)
`{"title_name":..., "nvn_major":..., "nvn_minor":...}`. The NVN API version is checked by the game against its build-time constant
(Super Mario Odyssey: major == 53, minor >= 13, found at `sead::GameFrameworkNx::initializeGraphicsSystem`; Link's Awakening accepts 55.15).
Without the right value the game silently skips graphics initialisation.

## Kernel scheduling note
Virtual cores are cooperative with priority preemption at svc entry (`threads.c: core_checkpoint`): a thread that holds a core and never issues an svc (a pure busy wait on
a variable) is not time-sliced, unlike the real kernel's 10 ms slices. No title spins like that today (the one earlier case, the applet message event that was never
de-asserted in LAS, was a service bug). If one appears the design is a watchdog thread that signals a core owner after 20 ms without an svc while a thread of equal or
better priority waits, with the handler allowed to switch only while the thread is in guest code. Not implemented: no evidence, and signal-based preemption risks deadlocks
inside the runtime.

## Debug aids
`NAPPLE_INPUT_SCRIPT="10:A,12:PLUS:0.3,30:L+R:0.8"` scripted key presses (`+` joins a chord that is pressed atomically; the script uses the bindings of input.json); `NAPPLE_INPUT_LOG=1` logs every change of player 1's merged state; `kill -USR1 <pid>` dumps every guest thread (state, wait, backtrace) without stopping; `NAPPLE_OP_MODE=docked|handheld`; `NAPPLE_AUDIO_VOLUME=0.05` with `NAPPLE_AUDIO_DEVICE=1` for audible tests at low volume; `NAPPLE_FE_OVERLAY=<tab>` opens the overlay at start and `NAPPLE_FE_VIEWSHOT=<png>:<secs>` renders it to a PNG; `tools/synth_keys.m` posts real CGEvents (see docs/changes/2026-10-23-input-audio-runtime-round2.md); `NAPPLE_SCREENSHOT=<prefix>` + `NAPPLE_SCREENSHOT_FRAMES=1,60,...` PNG of presented frames;
`NAPPLE_WATCH=main+0x24741f8` logs every guest write to a location; `NAPPLE_PEEK`, `NAPPLE_FS_TRACE=1`, `NAPPLE_WAIT_SECS`, `NAPPLE_MOVIE_HOST=0`, `NAPPLE_AUDIO_DEVICE=1`.

## Kernel, loader, nvdrv and fs additions (2026-10-24)
Untested on macOS (developed and unit tested on Linux, see docs/changes/2026-10-24-loader-kernel.md). Status per item: **implemented**
(semantics of the public Horizon kernel / system modules), **partial** (limits listed).

Kernel svcs:
- 0x40 CreateSession, 0x41 AcceptSession, 0x43 ReplyAndReceive, 0x44 ReplyAndReceiveWithUserBuffer, 0x70 CreatePort, 0x71 ManageNamedPort,
  0x72 ConnectToPort; SendSyncRequest(WithUserBuffer) and ConnectToNamedPort on guest objects (`ksession.c`, message copy `kmsg.c`):
  **partial**. Guest-to-guest IPC between threads of the game. One address space and one handle table, so copy and move handles pass through
  with the sender's handle values (a copy handle is shared, not duplicated), A/B/W descriptors keep their addresses, X data is copied into the
  receiver's receive list for list modes 2 and above (modes 0/1 keep the sender's address), the pid field gets 0x80. A server session is
  signaled while a request waits and none is being served, or once its client is closed.
- 0x20 SendSyncRequestLight, 0x42 ReplyAndReceiveLight: **partial**. Seven words in w1..w7; ReplyAndReceiveLight replies first when bit 31 of
  w1 is set (the flag position is taken from the public kernel sources, not checked on hardware).
- 0x36 SynchronizePreemptionState: **partial**. Clears the TLR interrupt flag and takes a deferred switch. Preemption only happens at svc entry
  (cooperative cores). A thread with a non-zero TLR disable count is pinned there: the kernel sets interrupt_flag, the switch waits for this svc.
- 0x32 SetThreadActivity (pause / resume now real), 0x33 GetThreadContext3: **partial**. A paused thread stops at its next svc boundary,
  without its core, until resumed; a thread blocked in an svc is stopped already. Pausing waits up to 50 ms for that. Because scheduling is
  cooperative, a thread spinning in guest code without svcs keeps running, and GetThreadContext3 on it answers InvalidState. The context comes
  from the gate's register area: x0..x28 (guest x18 from its slot), fp, lr, sp, pc = svc site + 4, nzcv, fpcr/fpsr, q0..q31, tpidr.
  GetThreadContext3 on the calling thread answers Busy.
- 0x2F GetLastThreadInfo: **partial**. The thread that held the caller's virtual core before it: {fp, sp, lr, pc}, its TLR, and flag 1 when it
  is inside an svc (else the frame of its last svc). NoThread when none, or with `NAPPLE_FREE_CORES=1`.
- 0x2E GetDebugFutureThreadInfo (InvalidHandle: an application has no debug objects), 0x3C KernelDebug, 0x3D ChangeKernelTraceState
  (no-ops): **implemented**.
- 0x4B CreateCodeMemory, 0x4C ControlCodeMemory (`kcode.c`, aliases in `memory.c`): **partial**. Argument, permission, size and state checks
  as in KCodeMemory. Map (RW, state CodeOut 0x15) and MapToOwner (R/RX, GeneratedCode 0x14) share the source heap pages through mach_vm_remap.
  Both addresses must be 16 KiB aligned (otherwise InvalidAddress), and alias addresses must lie in the free parts of the guest reservation.
  Code generated at run time runs without the AOT rewrite, so it must not use svc, x18 or tpidr/tpidrro. The source heap stays accessible.
  QueryMemory reports the aliases but does not trim the surrounding free range.
- 0x50 CreateSharedMemory: **implemented**. Size and permission checks; zeroed host pages; the remote permission is not enforced.

Loader:
- `ldr:ro` / `ro:1` (`ro.c`, checks in `nro.c`): **partial**. RegisterProcessHandle, RegisterModuleInfo / RegisterProcessModuleInfo,
  UnregisterModuleInfo, MapManualLoadModuleMemory, UnmapManualLoadModuleMemory, with the ro module's checks and results. NRR checks: magic,
  size, program id and mask, kind (User for ldr:ro, JitPlugin for ro:1), hash list bounds; signatures are not verified. NRO checks: header,
  sizes as passed, page-aligned contiguous sections, sha256 listed in a registered NRR, not already loaded. What gets mapped is the image
  `napple build` rewrote ahead of time from every `*.nro` in the RomFS (`<data-dir>/nro/<module id>.{patched.bin,tables}`, manifest key
  `nro`), found by module id. It is relocated like the static modules (host overrides first, then every loaded module, NROs included) and its
  gates join the dispatch index. An NRO without such an image stops the runtime (`NAPPLE_IPC_LENIENT=1`: InvalidNro). Closing the session
  unloads its modules. The guest's own nn::ro relocation pass then runs over the mapped image. RELA entries are idempotent, but an import the
  runtime bound to a host override may be rebound by that pass to the guest's original function (the SDK's behaviour here is unchecked).
  The guest's bss buffer is unused (bss is part of the image). Like the static modules, an NRO whose text end and data start share a 16 KiB
  page fails to seal ("text overlaps data").
- Multi-module: up to 128 modules (was 31), slots of unloaded NROs are reused, and the gate index is rebuilt and published atomically on
  load/unload.

nvdrv host1x channels (`/dev/nvhost-nvdec`, `-vic`, `-nvjpg`, `-msenc`, host1x classes 0xF0 / 0x5D / 0xC0 / 0x21): **partial**.
'H' SET_NVMAP_FD, SET_TIMEOUT; SUBMIT (command buffers must lie in live nvmap handles; syncpoint increments land at once; fence thresholds
returned); GET_SYNCPOINT (allocated per channel and index from id 32 up, released on close); GET_WAITBASE (0); SET_SUBMIT_TIMEOUT;
MAP/UNMAP_CMD_BUFFER (one stable 32-bit device address per nvmap handle). Bad sizes, unknown handles and out-of-range indexes answer
BadParameter; other ioctls stay NotImplemented and are listed in the summary. The SUBMIT layout changed to the documented one: syncpoint
increments are 0x14 bytes and fences are u32 thresholds (earlier code used 0x10 and {id, value}). Nothing is decoded; video stays with movie.c.

fsp-srv save data (`fs.c`): **partial**.
- 21/25/28 Delete (by id, by space and id, by attribute), 22/23/35 CreateSaveDataFileSystem (plain / by system save id / with hash salt),
  32 ExtendSaveDataFileSystem (sizes only grow), 33 DeleteCacheStorage, 34 GetCacheStorageSize, 52 OpenSaveDataFileSystemBySystemSaveDataId,
  57/58 ReadSaveDataFileSystemExtraData(BySaveDataSpaceId), 59/66 WriteSaveDataFileSystemExtraData(WithMask), 60/61/62 OpenSaveDataInfoReader
  (all / by space / cache only; ISaveDataInfoReader::ReadSaveDataInfo), 69/70/71 extra data by attribute (read, masked write, masked read).
- Each save keeps its extra data (0x200 bytes) and id in `<root>.napple-meta`. Ids come from the root path, stable across runs but not the
  console's. A save opened before it was created gets an implicit record, and a later Create fills it in and succeeds. Create on a real
  record answers PathAlreadyExists.
- IFileSystem GetTotalSpaceSize / GetFreeSpaceSize of a save created with a size: that size, and size minus 16 KiB blocks in use. Other
  file systems still report 1 GiB. Quotas are reported, not enforced on writes.
- Temporary storage is emptied at its first mount in a process. Bcat storage is a plain directory, as before. 53
  OpenReadOnlySaveDataFileSystem is now read-only and answers TargetNotFound for a save that does not exist (it used to mount writable and
  create it).
- Argument order: commands taking (space, attribute) accept either order (space-first with zero padding, or attribute-first), since SDK
  versions differ.
- Journal review fixes: Commit clears the dirty flag before the snapshot under the journal lock, so a write racing with the commit is not
  lost from the report, and a failed commit keeps it. A failed rename of the new snapshot also removes the temporary. CleanDirectoryRecursively
  marks the root dirty. Deleting a save resets its journal, so a recreated root starts fresh. A 17th transactional root is logged (it is not
  transactional).

Tests: `make -C runtime test-host` (any POSIX host, e.g. Linux) builds the portable kernel, loader and service code with `tests/host`
stand-ins: test_kernel_ipc, test_loader, test_nvdrv, test_fs_savedata, test_fs_journal.

## Services added on branch `services-2026-10-24` (UNBUILT: never compiled or run on macOS)
Everything in this section was written on Linux, where `runtime/` does not build. It was syntax-checked with stub headers and its portable logic was
unit-tested against an IPC mock (`tools/linux-check/`), but **none of it has been built or run on macOS, and no command id, argument layout or result
description was checked against a console or a title** (they are the author's recollection of the public documentation). The compatibility matrix and
`%Analysis` must not count these as implemented until the verification in `docs/changes/2026-10-24-services-VERIFY.md` has been done.
Status words: *real* = state or logic behind the answer, covered by a unit test; *partial* = some commands real, the data source or the rest default;
*stub* = accepted with a safe offline default. Services are only reachable when the game's NPDM lists them.

| Service | Status | Notes |
|---|---|---|
| `mii:u` | partial | read-only database: the local player's Mii plus six default Miis (CharInfo 0x58); GetCount, Get, Get1, UpdateLatest, BuildRandom (a default of the requested gender), BuildDefault, FindIndex, GetIndex; no editing, no StoreData variants |
| `caps:su` | partial | every save call succeeds and adds an in-memory album entry without a file (64 max, oldest dropped); refused after `SetScreenShotPermission(Disable)` |
| `caps:u` | partial | in-memory album: lists saved entries, size/load of one yield 0 bytes, delete removes it, unknown entries report not found (host tests only, unverified on Mac) |
| `psm` | partial | battery percentage, charger type, voltage state, enough-power, charging flags from a configured host situation (docked = AC; `NAPPLE_BATTERY="NN[,ac|battery]"`), not the Mac's battery |
| `lbl` | real | brightness (clamped), applied brightness, switch, dimming, auto brightness, VR brightness; nothing reaches the display; no light sensor |
| `ncm` | partial | in-memory content-meta database (Set/Get/Remove/Has/HasAll/List/ListApplication/GetLatestContentMetaKey/GetSize/GetPatchId) and content storage (placeholders, Register, Has, Delete, counts, sizes); starts empty, nothing persisted, no content files (GetPath is empty); unverified layouts; GetContentIdByType/ListContentInfo/HasContent/ListContentMetaInfo/LookupOrphanContent stay stubs |
| `ns:am2`, `ns:ec` | partial | language conversion/selection with regional fallbacks and free/total space (statvfs of the save volume) are real; no application records; control data not found; e-commerce fails offline |
| `ns:vm` | stub | never needs a vulnerability update |
| `sfdnsres` | partial | error strings real; every lookup fails like a resolver with no server |
| `spsm` | stub | always awake |
| `bcat:u` | partial | name validation and progress record real; the delivery cache is always empty, sync finishes at once with the offline result |
| `ldn:u` | partial | initialize/open/create/station state machine with legal-transition checks and a state-change event; no radio: scans empty, connect fails |
| `irs` | stub | camera handles and a zeroed shared memory; processors report sensor not connected |
| `audin:u` | partial | one input device, parameter negotiation, buffer queue; appended buffers are zero-filled (silence) and complete at once |
| `hidbus` | stub | no device is ever found |
| `pdm:qry` | partial | play statistics of the running title (one launch, minutes since start); other titles and events empty |
| `set:fd` | real | in-memory settings items with key iterator; not persisted and not read back by `set:sys` |
| `olsc:u`, `ectx:aw`, `fgm`, `btm:u`, `ovln:snd` | stub | command ids are guesses; unlisted ids fail loudly |

Depth changes to existing services (IPC_STUB rows before → after, from the command tables): `appletOE` 87 → 55 (the guest's settings are stored and read
back: master volumes, VR mode, auto sleep, idle detection, screenshot permission, cpu boost, home-button blocking, ...); `hid` 30 → 21 (six-axis fusion and
zero drift per handle, player LED pattern, joy hold type, handheld mode, home-button protection, vibration permission now silences rumble, communication
mode, styles read back); `time:*` 12 → 8 (LoadTimeZoneRule and rule-aware ToCalendarTime/ToPosixTime, ClockSnapshot 400/401, 500/501, automatic
correction, device location = host IANA zone); `offline.c` stays 146 stub rows but `bsd:u` has a descriptor table with per-call errno, `nifm` requests
finish rejected with a result, `ssl` handshakes fail, `friend` notification Pop on an empty queue fails; `misc.c` (aoc, pctl) unchanged.

## aoc:u and the error / player select / controller library applets (applets-dlc-2026-10-24-plan5)

Supersedes the `aoc:u` row above (stub) and the library applet answers; status of everything here is *tested* on Linux against the IPC mock, *unbuilt* on macOS.

* **aoc:u** reads the `dlc` array of `<data-dir>/manifest.json` (written by `napple build`: `title_id`, `version`, `dir` relative to the manifest, `romfs`, `default_enabled`). Entries with `default_enabled` false, a title id outside `application id + 0x1000 + 1..0xfff`, or a duplicate index are dropped. `ListAddOnContent` returns the 1-based indices (title id minus the base id) sorted; `PrepareAddOnContent(index)` succeeds only for a kept index. No DLC (no array, bad JSON, no manifest) keeps the old answers (count 0, empty list, Prepare fails). Mounting: `fsp-srv` command 30 `OpenDataStorageByDataId(u8 storage, u64 data id)` serves the DLC's `romfs.bin` as a read-only `IStorage` (same object as the base RomFS); unknown ids answer `R_FS_NOT_FOUND_DATA`. Core: `aoc_core.[ch]`; manifest reading and the commands: `misc.c`. Command layouts and the `R_AOC_NOT_FOUND` result (module 2, description 1) are [unverified].
* **Library applets** (`applet.c`, answers in `applet_lib_core.[ch]`): all three are answered as if the user confirmed with the one profile / pad, so a headless run continues.
  * error (0xe): dismissed (success, no output); the header type, jump flag and first argument word are logged and put in the run summary (`summary_note`). A jump-to-settings request is not followed.
  * player select (0x10): success with the only profile (`g_user_id`): output `{u64 result = 0; u8 uid[16]}` (0x18). Cancel (result 2) exists in the core but nothing selects it yet.
  * controller support (0xc): `player_count_min..max` from the argument header (unset = 1..4), players = min clamped to max; single mode reports npad 0 (Player 1) as the selected id, otherwise `0xffffffff`. Output `{s32 players; u32 selected_id; u32 result = 0}` (0xC).
  Struct layouts follow libnx as remembered and are [unverified] against a title.

`audrec:u` (audio-small, 2026-10-24, unbuilt on macOS): `OpenFinalOutputRecorder` returns an `IFinalOutputRecorder` that records silence (48 kHz stereo PCM16); appended buffers are zero-filled and complete at once, with end times as a running frame count. Command ids and layouts from memory, unverified. Core: `audrec_core.[ch]`.

H.264 movies (2026-10-24, media-h264): `movie.c` also hosts `video/avc` decoders through VideoToolbox (`src/vt_h264.c`). Annex B or AVCC access units, SPS/PPS taken in-band (or csd via `vth264_config`), output packed NV12 into the same slots/pacing as VP9, B-frame reordering by pts (depth 2; 0 for baseline). The DecoderType value for AVC (0) is inferred, not confirmed: `NAPPLE_MOVIE_H264_TYPE` overrides. `vt_h264.c` also has `jpeg_decode_rgba` / `jpeg_encode_rgba` (ImageIO); not yet wired to nvjpg or caps.

ac-services (2026-10-24, plan9): `fsp-srv` cmd 202 `OpenDataStorageByDataId` shares cmd 30's handler: DLC data ids resolve through `aoc_data_id_romfs`; any other id is a system data archive served from `<data-dir>/sysdata/<id as 16 hex>.bin` when that file exists, else `NOT_FOUND_DATA` (0x7d202). Nothing creates `sysdata/` yet, so system data (e.g. id 0100000000000823, storage 3, asked by the v0 AC build) is still missing. `ICommonStateGetter` cmd 900 `SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled` is accepted as a no-op (stub). Tests: `test_svc_fs`, `test_svc_applet`.

sysdata-firmware (2026-10-24, plan10): `napple build --firmware` now writes every firmware data archive for title ids 0100000000000800-0FFF that has a RomFS (≤64 MiB; 40 on 22.1.0, ~300 MB) as `<title dir>/sysdata/<id16hex>.bin` (the raw RomFS image). fs.c `OpenDataStorageByDataId` looks in `<data-dir>/sysdata/` then `<data-dir>/../sysdata/` (data-dir is the build-id directory). AC v0 now gets 0100000000000823 (98324 bytes) and boots past it; the next stop is an SDK abort after `nfp:user` opens (SetTerminateResult 0x2a2).
