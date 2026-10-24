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
| `aoc:u` | stub | no add-on content yet (DLC mounting is a packaging task) |
| `pctl` | stub | unrestricted device |
| `time:u/s/a` | implemented | clock contexts in the shared memory block, time zone = host |
| `hid` | implemented / stubs | shared memory + configuration through `input/` (libnapple_input); SendVibrationValue(s) / GetActualVibrationValue / GetVibrationDeviceInfo drive CoreHaptics rumble; xpad/gesture stubs |
| `vi:u` | implemented / stubs | display, layer (android Parcel native window), vsync event thread; IHOSBinderDriver answers every IGraphicBufferProducer transaction with a dummy success (the SDK of Super Mario Odyssey creates an android::Surface eagerly; NVN never uses it) |

| `pl:u` / `pl:s` | implemented | shared fonts from `<app-support>/system/fonts` (see "Shared fonts"); zero-size fonts when absent |
| `audren:u`, `audout:u`, `hwopus` | implemented | over libnapple_audio; `GetReleasedAudioOutBuffer` zero-fills unused tags (the SDK returns the first tag without checking the count); hwopus calls `guest_write_prepare` on the PCM buffer |
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
