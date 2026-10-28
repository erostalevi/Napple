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
| `apm` | implemented | per-mode performance configuration remembered; every id accepted in both modes (the docked/handheld marks of the unverified clock table in `apm_core.c` are documentation only: Link's Awakening sets a "docked" id in handheld mode and stops if refused); CPU overclock flag read back; host never changes its own speed |
| `appletOE` | implemented / stubs | proxy with ICommonStateGetter, ISelfController, IWindowController, IAudioController, IDisplayController, ILibraryAppletCreator (storage), IApplicationFunctions; focus message queued at launch; stubs: sleep locks, screenshot permission, home button, recording, media state |
| `fsp-srv` | implemented | RomFS as IStorage over `romfs.bin`; save data and sdcard as host directories; save data of type account/device/system/cache is transactional (see "Save data"); stubs: access log, auto save data creation, free space (1 GiB) |
| `set` / `set:sys` | implemented | language list, region from the host language (`NAPPLE_REGION` overrides), colour set (`NAPPLE_COLOR_SET`), time zone name (`NAPPLE_TIMEZONE`, default UTC), firmware 18.1.0 (`settings_core.c`) |
| `acc:u0` | implemented / stubs | one offline user; profile image calls stubbed |
| `aoc:u` | tested | add-on content from the `dlc` array of `manifest.json`; DLC romfs mounted through `fsp-srv` command 30 (see the aoc:u section below) |
| `pctl` | implemented | parental controls off: no restriction, safety level 0, every confirm-permission call succeeds (documented answers; ids unverified) |
| `time:u/s/a` | implemented | clock contexts in the shared memory block, time zone = host |
| `hid` | implemented / stubs | shared memory + configuration through `input/` (libnapple_input); SendVibrationValue(s) / GetActualVibrationValue / GetVibrationDeviceInfo drive CoreHaptics rumble; every row is implemented: debug pad (player 1's pad), gesture (recogniser in `input/src/gesture.c` over the touch stream), six-axis at-rest from the sensor sample, npad assignment (deactivate, disconnect, swap, merge, LR mode), capture assignment, keyboard lock keys, touch resolution, console six-axis record, active vibration device list, xpad = none; section offsets for gesture/console six-axis and the debug pad state layout are unverified |
| `vi:u` | implemented / stubs | display, layer (android Parcel native window), vsync event thread; IHOSBinderDriver answers every IGraphicBufferProducer transaction with a dummy success (the SDK of Super Mario Odyssey creates an android::Surface eagerly; NVN never uses it) |

| `pl:u` / `pl:s` | implemented | shared fonts from `<app-support>/system/fonts` (see "Shared fonts"); zero-size fonts when absent |
| `audren:u`, `audout:u`, `hwopus` | implemented | over libnapple_audio; audout buffer count / played samples / flush are real, audio device events fire on `audio_device_changed()`, manual-execution renderer renders on `ExecuteAudioRendererRendering`, the renderer's performance frame follows the v1 (REV1-4, 0x10 header) / v2 (REV5+, 0x20 header with `PERF` magic) layout (unverified); `GetReleasedAudioOutBuffer` zero-fills unused tags (the SDK returns the first tag without checking the count); hwopus calls `guest_write_prepare` on the PCM buffer |
| `audin:u` | implemented (silent) | over libnapple_audio (`napple_audin_*`): one device "BuiltInHeadset", OpenAudioIn(Auto), IAudioIn 0-6, 8, 9, 11-14; buffers are filled in real time with silence because no capture source is installed (no microphone access, no `NSMicrophoneUsageDescription`). `OpenAudioInProtocolSpecified` (5) and the UAC appends (IAudioIn 7, 10) are not implemented |
| `nvdrv`, `nvdrv:a/s/t` | partial | `/dev/nvmap` (create/alloc/free/param/get_id/from_id as bookkeeping on guest memory), `/dev/nvhost-ctrl` (syncpoints, events), host1x channels `nvhost-nvdec/vic/msenc/nvjpg` parse SUBMIT command buffers as host1x streams and run them on register-level engines (NVDEC to a `DecoderBackend`, VIC NV12 to RGBA, NVJPG real JPEG decode; see "Media services" at the end); submits complete instantly and syncpoints advance. Movie playback is still decoded by `movie.c`, see below. Every other device/ioctl is refused and listed in `missing_ipc` as `nvdrv:ioctl <path> <cmd>` |
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
### Timing: three rates (2026-10-27, fps-unlock-timing)
Three rates are kept apart, and only the first two can ever change what the game feels like:
1. **Game clock / guest cadence**: `timescale.c`. The virtual vsync of `vi.c` fires every `TS_GAME_VSYNC_NS` (1/60 s) divided by the Speed control and nothing else
   (`timescale_vsync_period_ns`); every guest clock follows the same scale. A title at swap interval 2 simply waits two vsyncs (30 Hz game logic). The deadline stepper
   `timescale_deadline_step` is shared by `vi.c` and the tests.
2. **Speed** (`speed_percent`, turbo, slow motion, `speed_mode`): the only control that makes the game faster or slower.
3. **Present rate** (`fps_mode`): a HOST property, `timescale_set_fps`. 0 game pacing, 1 match the display refresh (ProMotion 120 Hz), 2 custom N (`fps_cap`, 15..999),
   3 uncapped (`CAMetalLayer.displaySyncEnabled = NO`). It decides which finished game frames the presenter shows and is never read by the guest clocks. Above the game's
   frame rate the display repeats the last image (frame generation, `mtl_interp.m`, can fill in-between images); real extra game frames need a per-title timestep patch
   (a later per-title package). Below it (custom 30) the presenter drops frames. `vsync` off lets the layer present as soon as an image is ready (tearing possible), it
   too never touches game speed. `gfx_vsync_period_ns()` is always the game's 16.67 ms. In `mtl_present.m` the guest-visible frame time is `swap interval / 60 / speed` in
   every mode; the headless path (no layer) therefore always runs the game at its real speed whatever `fps_mode` says.
Settings: `settings.json` has `fps_mode` (+ `fps_cap` as the custom rate). Files from before it stored `fps_cap` as a rate the guest was paced by (speed scaled with it):
they migrate on load (0 -> game pacing, 1000 -> uncapped, N -> custom N and, when the file has no `speed_percent`, Speed = N/60 so the old feel is kept explicitly).
Headless: `NAPPLE_FPS_MODE=0..3`, `NAPPLE_FPS_HZ=<N>` (mode 2); `NAPPLE_SPEED=<factor>` is the speed. `NAPPLE_FRAME_TRACE=<file.csv>` writes `kind,real_ns,game_clock_ns` for
every guest vsync (V) and presented frame (P); `tools/fps/frame_trace.py` runs the title x mode matrix and asserts game clock 1.00 +-0.02 per wall second, guest vsync 60 Hz
+-3 % and presents never above vsync. Unit test: `runtime/tests/test_timescale.c` (10 simulated seconds at present rates 30/60/144/999/display/uncapped: identical vsync
count and game clock, +-0.1 %). The statistics overlay (F9) shows `game` fps (guest frames) and `shown` fps (presented images) separately.
## Shared fonts (`pl:u`)
The system fonts are Nintendo data and are never shipped. Extract them once from the user's own firmware:
`napple build --dump-fonts <Firmware.zip|dir> -k <prod.keys> -o "~/Library/Application Support/Napple/system/fonts"` (system data titles
0100000000000810..0815; each .bfttf is decoded: every u32 XOR 0x06186249, then {magic 0x18029A7F, u32 big-endian size, TTF}). The service lays the
fonts out in a 0x1100000-byte shared memory as {0x18029A7F, size ^ 0x49621806, TTF} and reports the offset of the TTF itself. Type mapping
(libnx PlSharedFontType): 0 std, 1 zh-CN, 2 zh-CN extension, 3 zh-TW, 4 Korean, 5 Nintendo extension, by file name.

## Host movie decoder (`runtime/src/movie.c`)
The SDK media library (`movie::Decoder`, Android MediaCodec over NvMM/NVDEC) is replaced for VP9 by libvpx: the constructor and the 17 virtual
methods are bound by symbol name through `host_symbols_register`, so the library's own vtable slots land on the host code; other decoder types
(Vorbis audio unless `NAPPLE_MOVIE_VORBIS=1`, H.264) are forwarded to the library's original code. Mode 1 (buffer) returns NV12 in the client's Buffer; mode 2 (NVN texture, used
by Link's Awakening) converts to RGBA and writes into the game's NVNtexture through `gfx_texture_write_rgba` (block-linear swizzle, dirty tracking).
Adapted from MacNativeGames (MIT), see THIRD_PARTY_NOTICES.md. H.264 is decoded through VideoToolbox (see the H.264 paragraph at the end).

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
`NAPPLE_INPUT_SCRIPT="10:A,12:PLUS:0.3,30:L+R:0.8"` scripted key presses (`+` joins a chord that is pressed atomically; the script uses the bindings of input.json); `NAPPLE_INPUT_LOG=1` logs every change of player 1's merged state; `kill -USR1 <pid>` dumps every guest thread (state, wait, backtrace) without stopping; `NAPPLE_OP_MODE=docked|handheld`; `NAPPLE_AUDIO_VOLUME=0.05` with `NAPPLE_AUDIO_DEVICE=1` for audible tests at low volume; `NAPPLE_FE_OVERLAY=<tab>` opens the overlay at start and `NAPPLE_FE_VIEWSHOT=<png>:<secs>` renders it to a PNG; `tools/dev/synth_keys.m` posts real CGEvents (see docs/archive/changes/2026-10-23-input-audio-runtime-round2.md); `NAPPLE_SCREENSHOT=<prefix>` + `NAPPLE_SCREENSHOT_FRAMES=1,60,...` PNG of presented frames;
`NAPPLE_WATCH=main+0x24741f8` logs every guest write to a location; `NAPPLE_PEEK`, `NAPPLE_FS_TRACE=1`, `NAPPLE_WAIT_SECS`, `NAPPLE_MOVIE_HOST=0`, `NAPPLE_AUDIO_DEVICE=1`.

## Kernel, loader, nvdrv and fs additions (2026-10-24)
Written and unit tested on Linux (see [the note](../archive/changes/2026-10-24-loader-kernel.md)); the host tests and later Mac rounds are recorded in the change archive. Status per item: **implemented**
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
increments are 0x14 bytes and fences are u32 thresholds (earlier code used 0x10 and {id, value}). Command buffers with a mapped address are executed since media-services (2026-10-26), see "Media services".

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

## Services added in the services round (2026-10-24)
This section was written on Linux, where `runtime/` does not build, and unit-tested against an IPC mock (`tools/linux-check/`). It was later built and its tests run on macOS
([verification](../archive/changes/2026-10-24-claude-services-verify-2026-10-24-plan5.md), [cleanup](../archive/changes/2026-10-24-cleanup-small-plan6.md)); the build and tests passed, but
**no command id, argument layout or result description was checked against a console**: they follow the public documentation as the author remembered it, and only commands a title
actually issued are verified by running it. Treat per-row statuses below as the authority.
Status words: *real* = state or logic behind the answer, covered by a unit test; *partial* = some commands real, the data source or the rest default;
*stub* = accepted with a safe offline default. Services are only reachable when the game's NPDM lists them.

| Service | Status | Notes |
|---|---|---|
| `mii:u` | partial | read-only database: the local player's Mii plus six default Miis (CharInfo 0x58); GetCount, Get, Get1, UpdateLatest, BuildRandom (a default of the requested gender), BuildDefault, FindIndex, GetIndex; no editing, no StoreData variants |
| `caps:su` | partial | every save call succeeds and adds an in-memory album entry (64 max, oldest dropped); a save with a 1280x720 RGBA input buffer is stored as JPEG (size and hash in the entry); refused after `SetScreenShotPermission(Disable)` |
| `caps:u` | partial | in-memory album: lists saved entries, delete removes it, unknown entries report not found; an entry with an image sizes to its JPEG, LoadAlbumFile gives the JPEG, 110 the decoded 1280x720 RGBA, 120 / session 5 the 320x180 RGBA thumbnail; entries without an image yield 0 bytes (tested on Linux, unverified on Mac) |
| `psm` | implemented | state-change events (session and charge-info) fire on `psm_host_power_changed()` and toggled emulation, controller power supply acquire/release counted; battery percentage, charger type, voltage state, enough-power, charging flags from a configured host situation (docked = AC; `NAPPLE_BATTERY="NN[,ac|battery]"`), not the Mac's battery |
| `lbl` | real | brightness (clamped), saved vs current vs applied brightness (Save/Load), switch, dimming, auto brightness, VR brightness and VR apply, ambient sensor value and reflection delay level read back; nothing reaches the display; no light sensor (`IsAmbientLightSensorAvailable` = false) |
| `ncm` | partial | in-memory content-meta database (Set/Get/Remove/Has/HasAll/List/ListApplication/GetLatestContentMetaKey/GetSize/GetPatchId) and content storage (placeholders, Register, Has, Delete, counts, sizes); starts empty, nothing persisted, no content files (GetPath is empty); unverified layouts; GetContentIdByType/ListContentInfo/HasContent/ListContentMetaInfo/LookupOrphanContent stay stubs |
| `ns:am2`, `ns:ec` | partial | language conversion/selection with regional fallbacks and free/total space (statvfs of the save volume) are real; no application records; control data not found; e-commerce fails offline |
| `ns:vm` | stub | never needs a vulnerability update |
| `sfdnsres` | partial | error strings real; every lookup fails like a resolver with no server |
| `spsm` | implemented | always awake (the host never sleeps: sleep requests return at once, no power message pending) |
| `bcat:u` | implemented | name validation, passphrase size check and progress record real; the delivery cache is always empty (a console with no network), sync finishes at once with the offline result, background tasks are accepted and never scheduled |
| `ldn:u` | partial | initialize/open/create/station state machine with legal-transition checks and a state-change event; no radio: scans empty, connect fails |
| `irs` | stub | camera handles and a zeroed shared memory; processors report sensor not connected |
| `audin:u` | partial | one input device, parameter negotiation, buffer queue; appended buffers are zero-filled (silence) and complete at once |
| `hidbus` | stub | no device is ever found |
| `pdm:qry` | implemented | play statistics of the running title (one launch, minutes since start); no play log is kept, so other titles and event queries answer empty like a freshly reset console |
| `set:fd` | real | in-memory settings items with key iterator; not persisted and not read back by `set:sys` |
| `olsc:u`, `ectx:aw`, `fgm`, `ovln:snd` | implemented (offline answers) | command ids are guesses; unlisted ids fail loudly. `btm:u` stays stub (BLE hardware simulation belongs to sim-online) |

Depth changes to existing services (IPC_STUB rows before → after, from the command tables): `appletOE` 87 → 55 (the guest's settings are stored and read
back: master volumes, VR mode, auto sleep, idle detection, screenshot permission, cpu boost, home-button blocking, ...); `hid` 30 → 21 (six-axis fusion and
zero drift per handle, player LED pattern, joy hold type, handheld mode, home-button protection, vibration permission now silences rumble, communication
mode, styles read back); `time:*` 12 → 8 (LoadTimeZoneRule and rule-aware ToCalendarTime/ToPosixTime, ClockSnapshot 400/401, 500/501, automatic
correction, device location = host IANA zone); `offline.c` stays 146 stub rows but `bsd:u` has a descriptor table with per-call errno, `nifm` requests
finish rejected with a result, `ssl` handshakes fail, `friend` notification Pop on an empty queue fails; `misc.c` (aoc, pctl) unchanged.

## aoc:u and the error / player select / controller library applets (applets-dlc-2026-10-24-plan5)

Supersedes the `aoc:u` row above (stub) and the library applet answers; status of everything here is *tested* on Linux against the IPC mock; built and tested on macOS in the later verification round.

* **aoc:u** reads the `dlc` array of `<data-dir>/manifest.json` (written by `napple build`: `title_id`, `version`, `dir` relative to the manifest, `romfs`, `default_enabled`). Entries with `default_enabled` false, a title id outside `application id + 0x1000 + 1..0xfff`, or a duplicate index are dropped. `ListAddOnContent` returns the 1-based indices (title id minus the base id) sorted; `PrepareAddOnContent(index)` succeeds only for a kept index. No DLC (no array, bad JSON, no manifest) keeps the old answers (count 0, empty list, Prepare fails). Mounting: `fsp-srv` command 30 `OpenDataStorageByDataId(u8 storage, u64 data id)` serves the DLC's `romfs.bin` as a read-only `IStorage` (same object as the base RomFS); unknown ids answer `R_FS_NOT_FOUND_DATA`. Core: `aoc_core.[ch]`; manifest reading and the commands: `misc.c`. Command layouts and the `R_AOC_NOT_FOUND` result (module 2, description 1) are [unverified].
* **Library applets** (`applet.c`, answers in `applet_lib_core.[ch]`): all three are answered as if the user confirmed with the one profile / pad, so a headless run continues.
  * error (0xe): dismissed (success, no output); the header type, jump flag and first argument word are logged and put in the run summary (`summary_note`). A jump-to-settings request is not followed.
  * player select (0x10): success with the only profile (`g_user_id`): output `{u64 result = 0; u8 uid[16]}` (0x18). Cancel (result 2) exists in the core but nothing selects it yet.
  * controller support (0xc): `player_count_min..max` from the argument header (unset = 1..4), players = min clamped to max; single mode reports npad 0 (Player 1) as the selected id, otherwise `0xffffffff`. Output `{s32 players; u32 selected_id; u32 result = 0}` (0xC).
  Struct layouts follow libnx as remembered and are [unverified] against a title.

`audrec:u` (audio-small, 2026-10-24): `OpenFinalOutputRecorder` returns an `IFinalOutputRecorder` that records silence (48 kHz stereo PCM16); appended buffers are zero-filled and complete at once, with end times as a running frame count. Command ids and layouts from memory, unverified. Core: `audrec_core.[ch]`.

H.264 movies (2026-10-24, media-h264): `movie.c` also hosts `video/avc` decoders through VideoToolbox (`src/vt_h264.c`). Annex B or AVCC access units, SPS/PPS taken in-band (or csd via `vth264_config`), output packed NV12 into the same slots/pacing as VP9, B-frame reordering by pts (depth 2; 0 for baseline). The DecoderType value for AVC is 1, read from the movie library ([note](../archive/changes/2026-10-24-h264-decoder-type.md)): `NAPPLE_MOVIE_H264_TYPE` overrides. `vt_h264.c` also has `jpeg_decode_rgba` / `jpeg_encode_rgba` (ImageIO); nvjpg and caps use the portable `jpeg_core` instead (below).

ac-services (2026-10-24, plan9): `fsp-srv` cmd 202 `OpenDataStorageByDataId` shares cmd 30's handler: DLC data ids resolve through `aoc_data_id_romfs`; any other id is a system data archive served from `<data-dir>/sysdata/<id as 16 hex>.bin` when that file exists, else `NOT_FOUND_DATA` (0x7d202). Nothing creates `sysdata/` yet, so system data (e.g. id 0100000000000823, storage 3, asked by the v0 AC build) is still missing. `ICommonStateGetter` cmd 900 `SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled` is accepted as a no-op (stub). Tests: `test_svc_fs`, `test_svc_applet`.

sysdata-firmware (2026-10-24, plan10): `napple build --firmware` now writes every firmware data archive for title ids 0100000000000800-0FFF that has a RomFS (≤64 MiB; 40 on 22.1.0, ~300 MB) as `<title dir>/sysdata/<id16hex>.bin` (the raw RomFS image). fs.c `OpenDataStorageByDataId` looks in `<data-dir>/sysdata/` then `<data-dir>/../sysdata/` (data-dir is the build-id directory). AC v0 now gets 0100000000000823 (98324 bytes) and boots past it; the next stop is an SDK abort after `nfp:user` opens (SetTerminateResult 0x2a2).

sim-online (2026-10-26, branch `claude/sim-online-2026-10-26-macos1`; supersedes the offline rows for `friend`, `nifm`, `bsd`, `nsd`, `ssl`, `prepo`, `nfp:user`/`nfc:user`, `sfdnsres` and `irs` above; status is **tested on Linux** against the IPC mock, not yet run on macOS or a title; all command layouts are from memory of the public documentation and unverified):

| Service | Status | Notes |
|---|---|---|
| `bsd:u`/`:s`/`:a` | implemented over [`OnlineProvider`](../guides/network-providers.md) | guest descriptors map to provider sockets (`bsd_core.c`); Socket, Select, Poll, Recv, RecvFrom, Send, SendTo, Accept, Bind, Connect, GetPeerName, GetSockName, GetSockOpt, SetSockOpt, Listen, Ioctl (FIONBIO), Fcntl (O_NONBLOCK), Shutdown, ShutdownAllSockets, Write, Read, Close; with the NullProvider the errno answers are the previous offline ones |
| `nifm:u`/`:a`/`:s` | implemented over the provider | Submit asks `net_connect`; Accepted/Free states, `GetCurrentIpAddress`, connection status, `IsAnyInternetRequestAccepted`; NullProvider rejects with "communication disabled" |
| `sfdnsres` | implemented over the provider | GetHostByName/Addr, GetAddrInfo, GetNameInfo (+WithOptions) serialise provider answers; NullProvider: TRY_AGAIN / EAI_AGAIN |
| `ssl` | implemented over the provider | handshake, Read, Write, Pending through `tls_*`; NullProvider: not connected |
| `friend:*` | partial | friend count/list/ids and `UpdateUserPresence` from the provider; notifications still empty |
| `nsd:u`/`:a`, `prepo:*` | provider hooks | `nsd_resolve` (identity by default), `report` (dropped by default) |
| `nfp:user`, `nfc:user`, `nfc:mf:u` | implemented over [`NfcTagSource`](../guides/hardware-simulators.md) | virtual amiibo state machine, structures, application area, events; no reader unless a frontend or `NAPPLE_AMIIBO` installs one (then the old "no device" answers stay) |
| `irs` | implemented over `IrFrameSource` | moment, clustering, image transfer on 320x240 frames; no source = sensor not connected as before |

IPC command depth (`IPC_IMPL` / `IPC_STUB` rows): `offline.c` 1 / 146 -> 106 / 80, `irs.c` 1 / 17 -> 11 / 7, `sfdnsres.c` 2 / 12 -> 10 / 4.

## Media services (2026-10-26, media-services; tested on Linux, unbuilt and unverified on the Mac)
Portable cores (`runtime/src/services/*_core.c`, no macOS headers; unit tests in `runtime/tests/test_svc_*.c` and `tests/host/test_nvdrv.c`):

| Piece | Files | What it does |
|---|---|---|
| host1x engines | `nvdrv_media_core.[ch]`, `nvdrv_media_regs.h` | Parses command buffers: SETCL (with register mask), INCR, NONINCR, MASK, IMM, RESTART, GATHER (command gather and register gather, depth 4), EXTEND; writes engine registers; EXECUTE (offset 0xC0) runs the engine. Malformed op or unmapped gather fails the SUBMIT (BadParameter); a failed decode is only counted and written to the status structure. Writes to registers outside the map are counted (`MediaStats.unknown_regs`) and summarised on close, so a real title's trace fixes the map |
| `nvdrv.c` wiring | `nvdrv.c` | SUBMIT reads each command buffer whose nvmap handle has a mapped address (`guest_ptr`), applies relocations (word = (target iova + offset) >> shift) to a copy, runs it on the channel's engine under the nvdrv lock. Handles without an address behave as before (accepted, nothing run). Surfaces are addressed by device address >> 8 (MAP_CMD_BUFFER iovas) |
| NVDEC (0xF0) | same | application id = codec (H.264 3, VP8 5, H.265 7, VP9 9), picture-info, bitstream and output surfaces (display buffer or per-picture-index slots), execute -> `DecoderBackend.decode`, status structure (error, width, height, macroblocks) |
| VIC (0x5D) | same | slot 0 NV12 -> RGBA/XBGR/ARGB with BT.601/BT.709, limited/full range, nearest scaling, pitch. Uses a **reduced Napple config structure** (9 words, `nvdrv_media_regs.h`), not the real one |
| NVJPG (0xC0) | same + `jpeg_core.[ch]` | baseline JPEG (all samplings, restart intervals, grey) to RGBA or NV12, optional scaling and pitch, status (error, size). Reduced pic-setup structure (6 words) |
| MSENC (0x21) | same | executes accepted, status says no bitstream |
| DecoderBackend | `decoder_backend.h`, `decoder_null_core.c` | frames in (codec, picture-info, bitstream, output planes), NV12 out; default null backend takes the size from the codec headers and writes a deterministic grey. The Mac VideoToolbox/libvpx backend is not written yet: it implements the struct and calls `decoder_backend_install` at start-up |
| Codec headers | `codec_headers_core.[ch]` | H.264 SPS (incl. cropping, high-profile scaling lists), VP9 uncompressed header, VP8 frame tag, H.265 SPS skeleton (size only) |
| Movie audio | `movie_audio_core.[ch]`, `third_party/stb_vorbis.c`, `movie.c` | Vorbis (raw packets wrapped into Ogg pages for stb_vorbis push-data; headers in-band or as Xiph csd-0) and Opus (libopus, `-DNAPPLE_HAVE_OPUS`) to interleaved s16 PCM. `movie.c` hosts type 5 / PCM / buffer mode decoders on it **only with `NAPPLE_MOVIE_VORBIS=1`** (`MOVIE_VORBIS_DEFAULT` 0 until a Mac run proves the SDK's calling pattern) |
| JPEG caps | `caps_core.[ch]`, `caps.c`, `jpeg_core` | see the caps rows above |

ioctl table of the multimedia channels (type 0 unless noted; all layouts as in the nvdrv.c comment): 'H' 0x01 SET_NVMAP_FD, 'H' 0x03 SET_TIMEOUT, 0x01 SUBMIT, 0x02 GET_SYNCPOINT, 0x03 GET_WAITBASE, 0x07 SET_SUBMIT_TIMEOUT, 0x09 MAP_CMD_BUFFER, 0x0A UNMAP_CMD_BUFFER.
Everything else on these devices is still refused by number (NotImplemented, listed in the summary). **The register offsets, the codec ids and the reduced VIC/NVJPG structures
are written from memory of the public driver sources and were never checked against a real title**; the Mac check is to run the four titles with `-v` (`NAPPLE_VERBOSE=1`),
read the `nvdrv` data lines and the "writes to registers outside the known map" summary, and fix `nvdrv_media_regs.h` (the only place offsets live).
Known limits: NVDEC output is pitch-linear NV12 (the hardware writes block-linear); decode runs inside the nvdrv lock; no H.264 parameter-set synthesis from the picture-info structure (the null backend never needs it, a real backend does).

Depth changes (runtime-depth, 2026-10-26, IPC_STUB rows before → after; scan "IPC: command depth" 52% → 68%): `hid` 24 → 0, `audio.c` 10 → 0, `bcat` 21 → 0, `psm` 9 → 0,
`lbl` 9 → 0, `settings` 4 → 0, `apm` 2 → 0, `pdm` 6 → 0, `misc.c` (pctl, aoc) 17 → 0, `olsc` 23 → 0, `spsm`/`ovln`/`ectx`/`fgm` → 0, `ns` 22 → 7, `ncm` 7 → 4, `time` 8 → 0,
`audin` 2 → 0, `ngc` 4 → 2. Rows that stay stubs on purpose are listed in `docs/archive/changes/2026-10-26-runtime-depth.md` with the reason (needs a hardware simulation,
a data drop, build-layout data, or belongs to another area).

small-gaps (2026-10-27, branch `claude/small-gaps-2026-10-27`; status **tested on Linux**, nothing run on macOS or a title; layouts from memory of the public documentation, unverified; supersedes the `audin:u`, `bcat:u` and `irs` rows above and the performance note of the `audren:u` row):

| Service | Status | Notes |
|---|---|---|
| `audin:u` | implemented | the first OpenAudioIn installs the capture source named by `NAPPLE_AUDIN_SOURCE` (silence, `tone:HZ[:AMP]`, `file:PATH`, `mic`; `audio/src/audin_source.c`). The microphone (`audin_mic.c`, an AUHAL input unit feeding a 48 kHz mono ring) is unbuilt on macOS and opt-in until the app has the usage description and entitlement |
| `audren:u` | implemented | the performance frame is kept in a history of `performance_frame_count` frames (1 when 0) until a RequestUpdate brings a buffer; whole frames are handed over oldest first and chained through next_offset. Header layout and modelled costs unchanged and unverified |
| `bcat:u` | implemented | delivery cache comes from a provider (`bcat_set_provider`; `NAPPLE_BCAT_DIR` = folder provider): sync requests succeed at once, Open/Read/GetCount, file Open/Read/GetSize/GetDigest (MD5) and EnumerateDeliveryCacheDirectory read from it. No provider = the offline answers as before. No server code in the repository |
| `irs` | implemented over `IrFrameSource` | ActivateIrsensor, SetFunctionLevel, ActivateIrsensorWithFunctionLevel, RunImageTransferExProcessor (the transfer processor) and RunIrLedProcessor are no longer stubs; tera plugin (310) and pointing/DPD (312) remain stubs |
| `hid` | implemented | GameCube, Palma and NES (Lark) styles are selected when the title announces them (alone, or by the player's binding style); GC and the retro pads report through the FullKey lifo, handheld Lark through the Handheld lifo, Palma through its own. The GC trigger lifo is not written (offset unverified) |

IPC command depth: `irs.c` 11 / 7 -> 16 / 2 rows (`IPC_IMPL` / `IPC_STUB`).

Depth changes (ipc-depth-rest, 2026-10-27, IPC_STUB rows before → after; supersedes the stale `mm:u`, `hidbus`, `btm:u` and `ncm` rows above): `offline.c` 89 → 3 (mm clock
request table, friend/nifm/nsd/prepo/ssl/nfc state; left as stubs: `DuplicateSocket`, `RecvMMsg`, `SendMMsg`), `btm` 14 → 0 (scan and connection state, still no radio), `hidbus` 12 → 0
(handle-checked "not connected" answers), `ldn` 10 → 0 (access point advertise data / accept policy / filter in `ldn_core.c`; network queries stay "not available"), `ns` 7 → 3
(application record list of the running title; `GetApplicationControlData` and the two content-path rows need build-layout data), `ncm` 4 → 0 (content infos parsed from the stored
meta by `ncm_core.c`), `sfdnsres` 4 → 0, `ngc` 2 → 0 (`ngc_core.c`; real matching with a user-supplied `NAPPLE_NGC_WORDLIST`, nothing is shipped), `push` 1 → 0. Inventory and reasons:
`docs/archive/changes/2026-10-27-claude-ipc-depth-rest-2026-10-27.md`. Layouts of the new rows are the author's memory of the public documentation (unverified); tested on Linux only.

### media-nvdrv (2026-10-27): Vorbis out-of-band headers, nvdisp-disp0, nvdrv rows, frozen DecoderBackend contract

Status of everything here: *tested* on Linux (`tools/linux-check`), *unbuilt* on macOS, never run against the SDK movie library or a real title.

| Piece | Where | What |
|---|---|---|
| Vorbis out-of-band headers | `movie_audio_core` `vorbispcm_set_headers_csd`, `movie.c` Configure | the format's `csd-0..2` buffers are read in Configure: one Xiph bundle, three single packets, or identification + setup (the empty comment header is synthesised, Android MediaCodec layout). A rejected header leaves the collection untouched. Audio packets before the headers are still dropped with one log line. **On by default** now (`MOVIE_VORBIS_DEFAULT 1`); `NAPPLE_MOVIE_VORBIS=0` is the opt-out |
| `/dev/nvdisp-disp0`, `-disp1` | `nvdrv_media_core` `nvdisp_*`, `nvdrv.c` `DEV_DISP` | query surface only, **proposed ABI** (no public nvdisp ABI): type `'D'`, nr 1 GET_MODE (1280x720@60 handheld), 2 GET_VBLANK, 3 WAIT_VBLANK (never blocks, strictly increasing), 4 GET_SYNCPT (26 / 27), 5 GET_STATUS. Other ioctls stay NotImplemented and are listed in `missing_ipc` |
| `nvdrv` command rows | `nvdrv.c` | `IPC_STUB` rows 7 -> 1: QueryEvent, MapSharedMemory, GetStatus, SetAruid, DumpGraphicsMemoryInfo (verbose log of the nvmap totals), InitializeDevtools are implemented answers with the reason in the table; `Unknown13` stays a stub (no public description) |
| DecoderBackend ABI 1 | `decoder_backend.h` | frozen: `open/close/decode` + optional `reset/configure`, `abi` field checked by `decoder_backend_install` (returns -1 on mismatch), call order and the synchronous in-decode-order rule are in the header comment |
| Parameter-set hand-off | `nvdrv_media_core` `nvdec_configure`, `codec_headers_core` | H.264 SPS+PPS found in the NVDEC bitstream and VP9 key-frame configs reach `configure()` once per change (reset first); `DecoderConfig` carries the NAL units, the Annex B form for `vth264_config` and the vpcC record. `MediaStats.configures` counts them. The writers `h264_build_sps/pps` exist for the day the NVDEC picture-info layout is known (SPS/PPS synthesis for streams without in-band sets); they are tested only as round trips through the parsers |

Tests: `test_svc_movie_audio` 37 checks, `test_svc_nvdrv_media` 115 checks (Linux).
## Library applet host (applets-host, 2026-10-27)

Supersedes the `appletOE` row above and the library applet notes of the plan5 section: `applet.c` + `applet_ae.c` went from 84 to 10 `IPC_STUB` rows (`appletOE` 68 → 7, `appletAE` 16 → 3; kept stubs with reasons are listed in the change note). Status: *tested* on Linux (`runtime/tests/linux/run_applet.sh`: `test_svc_applet`, `_libs`, `_host`, `_ae`); *unbuilt* on macOS. Layouts of rows marked [unverified] in the sources are the author's memory of the public documentation.

* **Handler registry**: `libapplet_register_handler(id, handler)` (`applet_lib_core.h`) answers a library applet id before any built-in answer; the handler sees the pushed and interactive storages and returns output storages on both channels.
* **Firmware applet table**: `libapplet_program_for(id)` maps applet ids 0x0A–0x1A to the firmware program ids (`0100000000001001`…); a firmware applet runs through the host (child process / in-process) when `libapplet_firmware_enabled` says so: the Mii Maker (0x12) by default, any other entry with `NAPPLE_FIRMWARE_APPLET_IDS=0x11,controller,…` or `NAPPLE_FIRMWARE_APPLETS=all`; `NAPPLE_FIRMWARE_APPLETS=0` turns all off. Without the firmware the built-in (HLE) answer is used.
* **Interactive channel**: `PushInteractiveInData` / `PopInteractiveOutData` / `GetPopInteractiveOutDataEvent` are real; a firmware applet gets `iin<N>.bin`, answers `iout<N>.bin` (`PopInteractiveInData` / `PushInteractiveOutData` of `applet_ae.c`); output storages are a queue (`out0..out7`), `GetResult` returns the applet's exit result (`result.txt`).
* **Built-in applets**: swkbd foreground and inline (background mode: replies over the interactive channel until Finalize), controller support (connected pads, min/max, `NAPPLE_CONTROLLER_SUPPORT=cancel|strict`), player select (profile list from `applet_set_profile_lister`, `NAPPLE_PLAYER_SELECT=<index>|cancel`), error applet (frontend hook `applet_set_error_hook`, default none → the generic alert).
* **State rows**: sleep lock + event, fatal section, window visibility / foreground rights / GPU time slice, auto sleep override, illuminance (`NAPPLE_ILLUMINANCE=<lux>` makes the sensor present), capture image slots (`applet_capture_set_source` registers the frame source; none = opaque black), host events (`applet_signal_gpu_error`, `applet_signal_friend_invitation`, `applet_display_resolution_changed`).
Depth changes (ipc-depth-rest2, 2026-10-27, IPC_STUB rows before → after, scan count over the owned files 29 → 2): `account` 8 → 0 (network-account rows routed through `OnlineProvider.account_info`,
NullProvider answers unchanged; `InitializeApplicationInfo*` remembered per connection), `vi` 8 → 2 (layer table, scaling modes, display enable, binder refcounts and the system/manager display
rows in `vi_core.c`; `TransactParcel`/`TransactParcelAuto` stay stubs: the graphics back end never uses the producer), `mii_img` 4 → 0 (in-memory image database with generated 96x96 faces,
`mii_img_core.c`), `ns` 3 → 0 (`GetApplicationControlData` and the content-path rows read `<data_dir>/control.nacp`), `settings` 3 → 0 (US key code map, `settings_keycode_map`), `offline.c` 3 → 0
(`DuplicateSocket` aliases the descriptor in `bsd_core.c`; `SendMMsg`/`RecvMMsg` marshal FreeBSD LP64 mmsghdr arrays), `ldn` network rows routed through the provider (`ONLINE_CAP_LDN`).
`bsd:u` `RegisterClient` still answers 0 (a nonzero id broke SMO boot). Layouts are the author's memory of the public documentation (unverified); tested on Linux only. See
`docs/archive/changes/2026-10-27-claude-ipc-depth-rest2-2026-10-27.md`.


### Web and album library applets (applets-rare, 2026-10-27)
* **Web / offline web (0x13, 0x17)**: `applet_web_core.c` parses the WebArgs TLV storage and answers `WebCommonReturnValue` (exit reason + last URL). Offline documents resolve under the HtmlDocument root (`applet_web_set_html_root` / `NAPPLE_HTML_ROOT`) and reach the frontend as `file://` URLs through `applet_ui_open_web`; with no hook the user leaves at once on the requested page. Legal-information and system-data documents are an error exit (firmware romfs).
* **Photo viewer / album (0x15)**: `applet_album_core.c` lists the caps album (`applet_album_set_source`), shows decoded images through `applet_ui_show_image`, and in select mode returns the picked entry. Both register through `libapplet_register_handler` (`applet_web_register`, `applet_album_register`). Layouts are [unverified].
IPC polish (ipc-polish, 2026-10-27, IPC_STUB rows 16 → 7, impl 1136 → 1145): `applet.c` 7 → 1 (kept: `GetSystemSharedLayerHandle`, only reachable after `IsSystemBufferSharingEnabled` says yes, which it never does), `applet_ae.c` 3 → 0 (`nfc:sys`/`nfp:sys` moved to `offline.c`: `CreateSystemInterface` returns the same simulated interfaces as the user services; the dead `miiimg` placeholder is gone, `mii_img.c` owns that service). Rows that were already the console's offline answer are now `IPC_IMPL` with the reason in a comment: `IsSystemBufferSharingEnabled` (failing result for an unprivileged process), `PopLaunchParameter`, `EnsureSaveData`, `GetPreviousProgramIndex` (-1), `NotifyMessageToHomeMenuForDebug` (no Home menu process, both applet files). `GetSaveDataSize` now reads `UserAccountSaveDataSize`/`JournalSize` from `control.nacp` (32 MiB each without it). Remaining stubs (7): `applet` 42, `nvdrv` 13 (no public description), `vi` `TransactParcel`/`TransactParcelAuto` (the graphics back end bypasses the producer), `fs` 1003/1005/1006 (fs.c was owned by another session in this batch: accepted no-ops/log mode, should become `IPC_IMPL` with a comment). `bsd:u` `RegisterClient` still answers 0. `tests/linux/known_calls_titles.txt` lists the commands the four reference titles call; `check_known_calls.py` fails when one disappears or is a stub (except the documented `vi` rows). See `docs/archive/changes/2026-10-27-claude-ipc-polish-2026-10-27.md`.

runtime-small (b6, 2026-10-27, Linux, tested not verified): `vi` `TransactParcel`/`TransactParcelAuto` are `IPC_IMPL` (per-binder IGraphicBufferProducer slot bookkeeping in `vi_core.c`, still no pixels), `hid_core` carries the npad style constants/helpers (system common policy now supports all pad styles incl. GameCube/Palma/NES). See `docs/archive/changes/2026-10-27-claude-runtime-small-2026-10-27-b6.md`.

VideoToolbox DecoderBackend (media-mac, 2026-10-27): `services/decoder_vt.c` installs a `DecoderBackend` named `videotoolbox` from a constructor (H.264 only through `vt_h264.c`; `NAPPLE_DECODER=null` keeps the null backend; host/Linux builds never compile it). `decode()` is synchronous: each request is decoded and flushed, so the frame of this access unit is in the guest surface when it returns. The nvdrv engine now understands the SDK's real stream shape: registers arrive as THI method/data pairs (`INCR 0x10 count 2`), DPB output slots are registers 0x10c+idx (luma) / 0x11d+idx (chroma), the H.264 sequence is described by the picture-info structure only (the slice NAL units carry no SPS/PPS), so `nvdec_synth_h264` writes the SPS/PPS from it (word map in `nvdrv_media_regs.h`). VIC: slot-0 input luma/chroma 0x100/0x101, config 0x1c1, output luma/chroma 0x1c8/0x1c9; with no readable config the execute copies NV12 to the NV12 output surfaces. `media_write_prepare` (set by `media_gfx_glue.c`) tells gfx before the host writes a surface.

Library applet UI (applets-frontend, 2026-10-27): the controller (0xc) and player select (0x10) applets ask the frontend through `applet_controller_ask` / `applet_player_select_ask` (hooks `applet_set_controller_ui`, `applet_set_player_select_ui`; -2 = no UI, keep the headless answer), the error applet through `applet_set_error_hook`, web through `applet_ui_open_web` and the album through `applet_ui_show_image` (answer 1 select, 0 next, -1 close; `applet_ui_album_selecting()` says whether the viewer may select). The Mac frontend registers sheets for all of them (`frontend/src/applet_sheets.m`, `register_applet_sheets` in `app.m`). Scripted answers win over any sheet: `NAPPLE_CONTROLLER_SUPPORT`, `NAPPLE_PLAYER_SELECT`, `NAPPLE_WEB_ANSWER` (back | closed | requested | error | n), `NAPPLE_ALBUM_PICK` (index | first | next | none), `NAPPLE_SWKBD_ANSWER`. See `docs/archive/changes/2026-10-27-claude-applets-frontend-2026-10-27.md`.
