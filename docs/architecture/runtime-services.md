# Runtime services and kernel surface

The runtime (`runtime/`) hosts a rewritten game in one process: the guest's AArch64 code runs natively, `svc` and counter reads go through
`runtime_gate` (rewritten ahead of time), and the services below answer the IPC the SDK issues. Services are implemented at IPC level
(HIPC/CMIF and TIPC, with domains), so SDK-version differences do not matter, except for the few host overrides of guest imports (NVN, `nv::`
graphics memory, the movie decoder) described in [Graphics](gfx.md) and under [Media](#media) below.

This page describes the current state. The history of each change is in the [change archive](../archive/changes/README.md).

## Policy

- A service or command that is not implemented stops the runtime with its name and id, records it in the compat summary (`missing_ipc`) and the
  trace. `NAPPLE_IPC_LENIENT=1` returns an error result instead of stopping, to enumerate more gaps in one run; `NAPPLE_SVC_LENIENT=1` does the
  same for svcs.
- Command tables mark each row `IPC_IMPL` (real answer, or the console's own offline answer with the reason in a comment) or `IPC_STUB` (a
  documented safe default). The remaining stub rows each carry their reason in the table: no public layout, a missing provider hook, or a host
  feature Napple lacks.
- The service manager refuses services outside the NPDM access list, like the real one (the SDK falls back, e.g. `pctl` then `pctl:s`, `pctl:r`,
  `pctl:a`). A service is only reachable when the game's NPDM lists it.
- Network-facing services answer through the [`OnlineProvider`](../guides/network-providers.md). With the default NullProvider they give the
  answers of a console with no network.
- Layout caveats: where a command layout was taken from public documentation and has not been seen in a title's traffic it is marked
  [unverified] in the source; layouts with no public description at all are marked **proposed**. Those marks are kept below where they apply.

## Kernel (svc)

| svc | Name | Status | Notes |
|---|---|---|---|
| 0x01-0x06 | SetHeapSize, SetMemoryPermission, SetMemoryAttribute, MapMemory, UnmapMemory, QueryMemory | implemented | MapMemory is a copying stack mapping |
| 0x07 | ExitProcess | implemented | |
| 0x08-0x10 | threads | implemented | see [Scheduling](#scheduling) |
| 0x11, 0x12, 0x17 | events | implemented | |
| 0x13, 0x14 | Map/UnmapSharedMemory | implemented | remap, or copy when not host-page aligned |
| 0x15 | CreateTransferMemory | implemented | |
| 0x16 | CloseHandle | implemented | |
| 0x18, 0x19 | WaitSynchronization, CancelSynchronization | implemented | |
| 0x1A-0x1D | mutex and condition variable | implemented | |
| 0x1E | GetSystemTick | implemented | 19.2 MHz, see [Frame pacing](#frame-pacing-and-timing) |
| 0x1F | ConnectToNamedPort | implemented | `sm:` and guest ports |
| 0x20 | SendSyncRequestLight | partial | seven words in w1..w7 |
| 0x21, 0x22 | SendSyncRequest(WithUserBuffer) | implemented | host services; guest sessions partial (below) |
| 0x24, 0x25 | GetProcessId, GetThreadId | implemented | |
| 0x26 | Break | implemented | reported with guest backtrace |
| 0x27 | OutputDebugString | implemented | |
| 0x29 | GetInfo | implemented | ids 0-28 |
| 0x2A, 0x2B | cache flush | implemented | no-op |
| 0x2C, 0x2D | Map/UnmapPhysicalMemory | implemented | |
| 0x2E | GetDebugFutureThreadInfo | implemented | InvalidHandle: an application has no debug objects |
| 0x2F | GetLastThreadInfo | partial | the thread that held the caller's virtual core before it: {fp, sp, lr, pc}, its TLR, flag 1 when it is inside an svc (else the frame of its last svc); NoThread when none, or with `NAPPLE_FREE_CORES=1` |
| 0x30, 0x31, 0x37 | resource limits | implemented | plausible constants |
| 0x32 | SetThreadActivity | partial | pause / resume are real: a paused thread stops at its next svc boundary, without its core, until resumed; a thread blocked in an svc is stopped already; pausing waits up to 50 ms. A thread spinning in guest code without svcs keeps running |
| 0x33 | GetThreadContext3 | partial | from the gate's register area: x0..x28 (guest x18 from its slot), fp, lr, sp, pc = svc site + 4, nzcv, fpcr/fpsr, q0..q31, tpidr. InvalidState for a thread still running guest code, Busy on the calling thread |
| 0x34, 0x35 | WaitForAddress, SignalToAddress | implemented | address arbiter |
| 0x36 | SynchronizePreemptionState | partial | clears the TLR interrupt flag and takes a deferred switch; a thread with a non-zero TLR disable count is not switched until it issues this svc |
| 0x3C, 0x3D | KernelDebug, ChangeKernelTraceState | implemented | no-ops |
| 0x40, 0x41 | CreateSession, AcceptSession | partial | guest-to-guest IPC (below) |
| 0x42 | ReplyAndReceiveLight | partial | replies first when bit 31 of w1 is set (flag position from the public kernel sources, not checked on hardware) |
| 0x43, 0x44 | ReplyAndReceive(WithUserBuffer) | partial | guest-to-guest IPC (below) |
| 0x45 | CreateEvent | implemented | |
| 0x4B, 0x4C | CreateCodeMemory, ControlCodeMemory | partial | `kcode.c`, below |
| 0x50 | CreateSharedMemory | implemented | size and permission checks; zeroed host pages; the remote permission is not enforced |
| 0x51, 0x52 | Map/UnmapTransferMemory | implemented | no-op: services read guest memory directly |
| 0x70-0x72 | CreatePort, ManageNamedPort, ConnectToPort | partial | guest ports |

Everything else is unsupported and reported by number and name.

**Guest-to-guest IPC** (`ksession.c`, message copy `kmsg.c`): IPC between threads of the game. There is one address space and one handle table,
so copy and move handles pass through with the sender's handle values (a copy handle is shared, not duplicated), A/B/W descriptors keep their
addresses, X data is copied into the receiver's receive list for list modes 2 and above (modes 0/1 keep the sender's address), and the pid field
gets 0x80. A server session is signaled while a request waits and none is being served, or once its client is closed.

**Code memory** (`kcode.c`, aliases in `memory.c`): argument, permission, size and state checks as in KCodeMemory. Map (RW, state CodeOut 0x15)
and MapToOwner (R/RX, GeneratedCode 0x14) share the source heap pages through `mach_vm_remap`. Both addresses must be 16 KiB aligned (otherwise
InvalidAddress), and alias addresses must lie in the free parts of the guest reservation. Code generated at run time runs without the AOT rewrite,
so it must not use svc, x18 or tpidr/tpidrro. The source heap stays accessible. QueryMemory reports the aliases but does not trim the surrounding
free range.

## Scheduling

- Virtual cores: three application cores; a thread holds one while it runs guest code (`NAPPLE_FREE_CORES=1` disables the limit). The main thread
  starts pinned to its NPDM ideal core.
- Preemption (`threads.c`): a slice thread plus SIGPROF while the thread is in guest code, and a checkpoint at svc exit otherwise. A
  better-priority thread waiting for a core takes it within a few ms.
- Equal-priority threads rotate every 10 ms **only at priority 59**, the preemption priority of the application cores (Horizon's
  RotateScheduledQueue with {59, 59, 59, 63}). Threads of other priorities keep their core against an equal-priority thread until they block or
  yield, as on hardware.
- A thread preempted by a better one keeps the head of its priority (it resumes before equal-priority threads that queued meanwhile); woken,
  yielding and rotated threads go to the back.

| Variable | Effect |
|---|---|
| `NAPPLE_SLICE_MS=<n>` | slice length (0 = no timer preemption) |
| `NAPPLE_SLICE_ALL=1` | every priority rotates (old behaviour) |
| `NAPPLE_PREEMPT_SIGNAL=0` | switch only at svc boundaries |
| `NAPPLE_THREAD_LOG=1` | log CreateThread / StartThread / SetThreadCoreMask / SetThreadPriority |
| `NAPPLE_THREAD_STATS=1` | per-thread switch and GetCurrentProcessorNumber counts in the quit-time thread dump |

## IPC services

One row per service; status words: *implemented* (state or logic behind every answer, unit tested), *partial* (some rows real, the rest
documented defaults), *stub* (accepted with a safe offline default).

### System, settings and account

| Service | Status | Notes |
|---|---|---|
| `sm:` | implemented | Initialize, GetServiceHandle; NPDM access list enforced |
| `lm` | implemented | log packets forwarded to the runtime log; the accessor is the retail no-op |
| `set` | implemented | language list and code, region from the host language (`NAPPLE_REGION` overrides), 11 GetDeviceNickName (`NAPPLE_DEVICE_NICKNAME`, default "Napple"), 12 GetKeyCodeMapByPort, US key code map (`settings_keycode_map`); language, region, quest flag and nickname follow the `set:sys` store and fall back to the host values |
| `set:sys` | implemented | keeps what Set* writes for the life of the process (`sysset_core.c`); colour set (`NAPPLE_COLOR_SET`), 53 time zone location name (`NAPPLE_TIMEZONE`, default UTC), 62 GetDebugModeFlag (retail), 77 nickname, 79 product model (Nx), serial number from `NAPPLE_SERIAL` (else `XAW00000000000`), firmware version 18.1.0 (`settings_core.c`) |
| `set:fd` | implemented | in-memory settings items with key iterator; not persisted and not read back by `set:sys` |
| `acc:u0` | implemented | one offline user, plus "Player N" users with `NAPPLE_PROFILES=<2..8>` (uid last byte + index; `account_core.c`), reported by GetUserCount / ListAllUsers / GetUserExistence / GetProfile and offered to player select; profile digest (6), save-data thumbnail store / clear (110, 111), TrySelectUserWithoutInteraction (52); `InitializeApplicationInfo*` remembered per connection; network-account rows (id token, authorization, guest login) go through `OnlineProvider.account_info`; async contexts follow the provider's account state (0x1907C for an unlinked user); open contexts 160 / 130 / 131 |
| `pctl` | implemented | parental controls off: no restriction, safety level 0, every confirm-permission call succeeds; keeps a local PIN code, the temporary unlock, the default rating organization (from `NAPPLE_REGION`), the free-communication list and the last-change time (`pctl_core.c`); ids per switchbrew: 1018 IsFreeCommunicationAvailable, 1035 GetCurrentSettings, 1061 / 1062 / 1065 stereo vision, 1451 StartPlayTimer |
| `time:u/s/a` | implemented | clock contexts in the shared memory block; time zone = host IANA zone (or UTC, or the zone picked with SetDeviceLocationName); clock operation event fires on SetCurrentTime / SetSystemClockContext; automatic correction kept; LoadTimeZoneRule and rule-aware ToCalendarTime / ToPosixTime; ParseTimeZoneBinary turns TZif data into a fixed-offset rule at the standard offset (DST dropped); ClockSnapshot 400 / 401, 500 / 501; ISteadyClock 100 GetRtcValue, 101, 102, 200 GetInternalOffset |
| `apm` | implemented | per-mode performance configuration remembered; every id is accepted in both modes (a title may set a "docked" id in handheld mode and stop if refused; the docked/handheld marks of the unverified clock table in `apm_core.c` are documentation only); CPU overclock flag read back; the host never changes its own speed |
| `psm` | implemented | state-change events (session and charge-info) fire on `psm_host_power_changed()` and toggled emulation; controller power supply acquire / release counted; battery percentage, charger type, voltage state, enough-power and charging flags from a configured situation (docked = AC; `NAPPLE_BATTERY="NN[,ac|battery]"`), not the Mac's battery; 18 |
| `lbl` | implemented | brightness (clamped), saved vs current vs applied (Save/Load), switch (also driven by applet SetLcdBacklighOffEnabled), dimming, auto brightness, VR brightness and VR apply, ambient sensor value and reflection delay level read back; 20 / 22, 26 EnableVrMode, 27-29; nothing reaches the display; no light sensor (`IsAmbientLightSensorAvailable` = false) |
| `spsm` | implemented | always awake (the host never sleeps: sleep requests return at once, no power message pending); 0-5 |
| `fgm` | implemented | offline answers (`fgm::IRequest`) |
| `ovln:snd` | implemented | per-sender queues |
| `pdm:qry` | implemented | play statistics of the running title (one launch, minutes since start), 4 / 5; events 8-12; QueryLastPlayTime (17) per requested title; 18 / 19; no play log is kept, so other titles and event queries answer empty like a freshly reset console |
| `notif:a` | implemented | up to 8 alarms with their parameters (`notif_core.c`); they never fire (no HOME menu) |
| `ns:am2`, `ns:ec` | implemented | language conversion / selection with regional fallbacks, free / total space (statvfs of the save volume), application record list of the running title, `GetApplicationControlData` and the content-path rows read `<data-dir>/control.nacp`; e-commerce fails offline |
| `ns:vm` | stub | never needs a vulnerability update |
| `ncm` | implemented | in-memory content-meta database (Set/Get/Remove/Has/HasAll/List/ListApplication/GetLatestContentMetaKey/GetSize/GetPatchId, 19 / 22 / 23) and content storage (placeholders, Register, Has, Delete, counts, sizes); content infos parsed from the stored meta (`ncm_core.c`); starts empty, nothing persisted, no content files (GetPath is empty); layouts [unverified] |
| `ngc` | implemented | `ngc_core.c`: the firmware NgWord archive (`sysdata/0100000000000806.bin`, about 3000 words, `ngc_words_add_ngword`) plus an optional user list `NAPPLE_NGC_WORDLIST`; nothing is shipped |
| `olsc:u` | implemented | offline answers; ids per switchbrew (0 / 10 / 13-16 / 18-27); async contexts follow the provider's account state; unlisted ids fail loudly |
| `ectx:aw` | implemented | 0 CreateContextRegistrar + IContextRegistrar::Complete, 1 CommitContext |
| `csrng` | implemented | host `arc4random_buf` |
| `mii:u`, `mii:e` | implemented | one Mii database for every title: `<support root>/system/mii/MiiDatabase.dat` (or `NAPPLE_MII_DIR`; the App Group container on iOS), up to 100 Miis plus the six defaults built in code (`mii_core.c`, CharInfo 0x58); `mii:e` adds the editor commands. See [system data folders](../reference/system-data-folders.md) |
| `miiimg` | implemented | in-memory image database with generated 96x96 faces (`mii_img_core.c`); ids 10-17 and 100-104 |
| `pl:u`, `pl:s` | implemented | shared fonts, see [Shared fonts](#shared-fonts-plu); zero-size fonts when absent |

### File system and content

| Service | Status | Notes |
|---|---|---|
| `fsp-srv` | implemented | RomFS as IStorage over `romfs.bin` (or the in-place `romfs.map`, or the mod overlay, see [Mods](#mods-runtime)); RomFS as a read-only IFileSystem (`romfs_read.[ch]`: GetEntryType, OpenFile -> IFile over the storage extent, OpenDirectory); content types (`FileSystemProxyType`): Rom and Data serve the RomFS of the program id (application, its update, a DLC, or a `sysdata/` archive), Control serves `control.nacp`, Manual serves `<data-dir>/html`, others "path not found"; data storages by program id (201, 205); 202 OpenDataStorageByDataId (below); 30 OpenGameCardStorage; save data and SD card as host directories ([Save data](#save-data)); save info by filter (67, 68); internal storage (64); commit ids (616); SD card / access log state; access-checked rows follow the title's `main.npdm` FS permission bits (PermissionDenied without the bit). Documented defaults: auto save data creation, the save data transfer family, game card, rights ids; 1003 / 1005 / 1006 are accepted no-ops |
| `aoc:u` | implemented | add-on content from the `dlc` array of `manifest.json` (below); 302 UpdateCachedList re-reads the manifest, 200 keeps the required ids, 13; 100 / 101 hand out an IPurchaseEventManager that keeps delivery targets (its event never fires, nothing can be popped); contents service manager; host add-on content setup (300, 301) is a documented default |
| `bcat:u` | implemented | delivery cache from a provider (`bcat_set_provider`; `NAPPLE_BCAT_DIR` = folder provider; the folder may be plain or hold the console's delivery-cache save layout `directories.meta` / `files.meta` / `files/`, detected automatically); sync requests succeed at once; Open/Read/GetCount, file Open/Read/GetSize/GetDigest (MD5) and EnumerateDeliveryCacheDirectory; progress `Done` is status 9; notifier, suspension, delivery list, 20101; name validation and passphrase size check. No provider: an always-empty cache (a console with no network). No server code in the repository |

**DLC.** `aoc:u` reads the `dlc` array of `<data-dir>/manifest.json` (written by `napple build`: `title_id`, `version`, `dir` relative to the
manifest, `romfs`, `default_enabled`). Entries with `default_enabled` false, a title id outside `application id + 0x1000 + 1..0xfff`, or a duplicate
index are dropped. `ListAddOnContent` returns the 1-based indices (title id minus the base id) sorted; `PrepareAddOnContent(index)` succeeds only
for a kept index. No DLC (no array, bad JSON, no manifest) keeps count 0, an empty list and a failing Prepare. Core: `aoc_core.[ch]`; manifest
reading and the commands: `misc.c`. The `R_AOC_NOT_FOUND` result (module 2, description 1) is [unverified].

**Data storages.** `fsp-srv` 202 `OpenDataStorageByDataId(u8 storage, u64 data id)` serves a DLC's `romfs.bin` (through `aoc_data_id_romfs`) as a
read-only IStorage. Any other id is a system data archive served from `<data-dir>/sysdata/<id as 16 hex>.bin`, then `<data-dir>/../sysdata/`
(data-dir is the build-id directory); otherwise `NOT_FOUND_DATA` (0x7d202). `napple build --firmware` writes every firmware data archive for title
ids 0100000000000800-0FFF that has a RomFS of at most 64 MiB (about 40 archives, ~300 MB on firmware 22.1.0) as `<title dir>/sysdata/<id16hex>.bin`
(the raw RomFS image).

### Input, display and audio

| Service | Status | Notes |
|---|---|---|
| `hid` | implemented | shared memory and configuration through `input/` (libnapple_input); SendVibrationValue(s) / GetActualVibrationValue / GetVibrationDeviceInfo drive CoreHaptics rumble (vibration permission silences it); debug pad (player 1's pad), gesture (recogniser in `input/src/gesture.c` over the touch stream), six-axis (fusion and zero drift per handle, at-rest from the sensor sample, accelerometer parameters / play mode, unaltered passthrough, seven six-axis sensor state, forced console six-axis, npad IMU), npad assignment (deactivate, disconnect, swap, merge, LR mode, SingleWithDestination in place), player LED pattern, joy hold type, handheld mode, home-button protection, communication mode, capture assignment, keyboard lock keys, touch resolution and emulation, digitizer, mouse library version, USB full-key, battery and interface queries (from the backend's active style), GC ERM rumble, permit-vibration sessions, active vibration device list, xpad-era commands (no xpad: empty lists, lifo handles refused), Palma operation model (handle = npad id of a pad reporting the Palma style; operations complete at once and signal the event; data layouts [unverified]); 133-136 follow the reference ids, Clear (136) takes only the aruid and clears every style. Styles: below |
| `hidbus` | implemented | handle-checked "not connected" answers; no device is ever found |
| `irs` | implemented over `IrFrameSource` | moment, clustering, image transfer on 320x240 frames; ActivateIrsensor, SetFunctionLevel, ActivateIrsensorWithFunctionLevel, RunImageTransferExProcessor, RunIrLedProcessor; tera plugin (310) and pointing / DPD (312) stay stubs; no source = sensor not connected |
| `nfp:user`, `nfc:user`, `nfc:mf:u`, `nfp:sys`, `nfc:sys` | implemented over [`NfcTagSource`](../guides/hardware-simulators.md) | virtual amiibo state machine, structures, application area, events; no reader unless a frontend or `NAPPLE_AMIIBO` installs one (then "no device"); the `:sys` interfaces return the same simulated interfaces as the user services |
| `btm:u` | implemented | scan and connection state, no radio; IBtmUserCore 1, 2, 19 BleDisconnect, 20 BleGetConnectionState, 21 AcquireBlePairingEvent, 22-37; 1 / 2 scan filter parameters are `IPC_STUB` zeros; everything needing a connection fails "not connected" |
| `vi:u` (`vi:s`, `vi:m`) | implemented | display, layer (Android Parcel native window), vsync event thread ([Frame pacing](#frame-pacing-and-timing)); layer table, scaling modes, display enable, binder refcounts and the system / manager display rows (`vi_core.c`); indirect layers use the same binder driver (103), 2460 sizes the image buffer, 2450 / 2451 image maps return a transparent frame; IHOSBinderDriver TransactParcel / TransactParcelAuto keep per-binder IGraphicBufferProducer slot bookkeeping but carry no pixels (the graphics back end never uses the producer; an SDK may create an `android::Surface` eagerly); ISystemDisplayService / IManagerDisplayService keep gamma, contrast, CMU, RGB range, content type, magnification, alpha and power state (the host window is not affected); 2102 ConvertScalingMode |
| `audren:u` | implemented | over libnapple_audio; manual-execution renderer renders on `ExecuteAudioRendererRendering`; audio device events fire on `audio_device_changed()`; IAudioRenderer 12 / 13 voice drop; IAudioDevice 13 answers the master volume (before 13.0.0) or the active output device name (13.0.0+), 14-21. Performance frames are kept in a history of `performance_frame_count` frames (1 when 0) until a RequestUpdate brings a buffer; whole frames are handed over oldest first, chained through next_offset, ending at a null header. Layouts: format 1 (REV1..4) header 0x18, entry / detail 0x10; format 2 (REV5+) header 0x30, entry / detail 0x18; PERF magic in both (tested per revision with `make -C audio test`; modelled costs [unverified]) |
| `audout:u` | implemented | buffer count, played samples and flush are real; `GetReleasedAudioOutBuffer` zero-fills unused tags (the SDK returns the first tag without checking the count); IAudioOut 12 / 13 volume is a real per-stream gain (`napple_audout_set_volume`, clamped to 0..1, applied before the mix) |
| `audin:u` | implemented | over libnapple_audio (`napple_audin_*`): one device "BuiltInHeadset", OpenAudioIn(Auto), 5 OpenAudioInProtocolSpecified, IAudioIn 0-14 including the UAC appends 7 / 10. The first OpenAudioIn installs the capture source named by `NAPPLE_AUDIN_SOURCE` (`silence`, `tone:HZ[:AMP]`, `file:PATH`, `mic`; `audio/src/audin_source.c`). Default (unset) is auto: the microphone (`audin_mic.c`, an AUHAL input unit feeding a 48 kHz mono ring) when the process may use it (generated apps carry `NSMicrophoneUsageDescription`), silence when permission is denied, cannot be asked, or the start fails; never a failure |
| `audrec:u` | implemented | `OpenFinalOutputRecorder` returns an `IFinalOutputRecorder` that records silence (48 kHz stereo PCM16); appended buffers are zero-filled and complete at once, with end times as a running frame count; 10 / 11. Core: `audrec_core.[ch]`; layouts [unverified] |
| `hwopus` | implemented | calls `guest_write_prepare` on the PCM buffer; hwopus 8 / 9 and IHardwareOpusDecoder 8 / 9 |
| `mm:u` | implemented | multimedia clock request table |
| `caps:su` | implemented | every save call succeeds and adds an in-memory album entry (64 max, oldest dropped); a save with a 1280x720 RGBA input buffer is stored as JPEG (size and hash in the entry); refused after `SetScreenShotPermission(Disable)` |
| `caps:u` | implemented | in-memory album: lists saved entries (also the 5-variant lists 144-147), delete removes, unknown entries report not found; an entry with an image sizes to its JPEG, LoadAlbumFile gives the JPEG, 110 the decoded 1280x720 RGBA, 120 / session 5 the 320x180 RGBA thumbnail; entries without an image yield 0 bytes; movie read streams (2001-2005) read entries stored as movies; 142 / 143 |
| `caps:dc` | implemented | 3001 DecodeJpeg through `jpeg_core` |

**Npad styles.** GameCube, Palma, NES / Famicom (Lark), SNES (Lucia), N64 (Lagon) and Mega Drive (Lager) styles are selected when the title
announces them (alone, or by the player's binding style); GC and the retro pads report through the FullKey lifo, handheld Lark through the Handheld
lifo, Palma through its own. GC trigger lifo at 0x4228 of the npad entry (libnx `HidNpadInternalState`; values clamped to 0x7fff), Lark / Lucia /
Lager type words at 0x43E0, `HidNpadButton` bits 28..34 (Palma, verification, HandheldLeftB, N64 C buttons). Device-type bits (libnx
`HidDeviceTypeBits`) for every style: Pro Controller FullKey, handheld HandheldLeft | HandheldRight, Joy-Con pair JoyLeft | JoyRight, single Joy-Con
its side, retro / GC / Palma their own bit; `NAPPLE_NPAD_DEVICE_TYPES=0` keeps the standard styles at 0. `hid_core` carries the style constants;
the system common policy supports all pad styles. Section offsets for gesture / console six-axis and the debug pad state layout are [unverified].
Tests: `make -C input test`.

### Network (through the OnlineProvider)

| Service | Status | Notes |
|---|---|---|
| `bsd:u`, `bsd:s`, `bsd:a` | implemented | guest descriptors map to provider sockets (`bsd_core.c`): Socket, Select, Poll, Recv, RecvFrom, Send, SendTo, Accept, Bind, Connect, GetPeerName, GetSockName, GetSockOpt, SetSockOpt, Listen, Ioctl (FIONBIO), Fcntl (O_NONBLOCK), Shutdown, ShutdownAllSockets(how), Write, Read, Close, DuplicateSocket (aliases the descriptor), 28 GetResourceStatistics, 29 RecvMMsg / 30 SendMMsg (FreeBSD LP64 mmsghdr arrays), 31 EventFd, core mask. `RegisterClient` answers 0 (a nonzero id broke a title's boot). NullProvider: every network call answers {-1, ENETDOWN} |
| `nifm:u`, `nifm:a`, `nifm:s` | implemented | Submit asks `net_connect`; Accepted / Free states, `GetCurrentIpAddress`, IGeneralService 15 SetWirelessCommunicationEnabled, 16 / 17 IsWirelessCommunicationEnabled and settings, 18 GetInternetConnectionStatus, 19 / 20, 28, 34, 40, 41, 43, 57, `IsAnyInternetRequestAccepted`; profile store, scan request (0 access points), sleep / wake / shutdown over live requests, telemetry event; IRequest GetResult, GetRequirement, GetRevision, GetAppletInfo, 16-21, 23-25 and Cancel ([netConnect](#network-connection-applet-0x0f)). NullProvider rejects with "communication disabled" |
| `sfdnsres` | implemented | GetHostByName / Addr, GetAddrInfo, GetNameInfo (+WithOptions) serialise provider answers; error strings; resolver options; Query / Fetch / CancelQuery. NullProvider: TRY_AGAIN / EAI_AGAIN |
| `ssl` | implemented | handshake, Read, Write, Pending, Peek, Poll through `tls_*`; verify errors and per-connection options; ISslConnection 19 FlushSessionCache, 20-21 renegotiation mode; context PKI and CRL ids kept; ISslContext 6; the certificate-list rows agree (an empty built-in list). NullProvider: not connected |
| `nsd:u`, `nsd:a` | implemented | `nsd_resolve` hook (identity by default; environment "lp1"), application environment type (default lp), GetSettingUrl |
| `friend:u` (+v/m/s/a) | implemented | `friend_core.c` keeps the console's per-user state: play history, in-person requests, own presence, the user setting with an installation friend code. Cache rows need a linked network service account (provider `account_info`) and answer from the provider's friend list; sync rows also need `ONLINE_CAP_FRIENDS` and a connection; `UpdateUserPresence` goes to the provider. Offline: "no network service account" |
| `prepo:u` (+a/m/s) | implemented | `report` hook (dropped by default); reports kept for ReadAll*; ids 10100-10107, 20100-20103, 30300, 90100 |
| `ldn:u` | implemented | initialize / open / create / station state machine with legal-transition checks and a state-change event; access point advertise data, accept policy and filter (`ldn_core.c`); configuration rows and client process monitor; network rows routed through the provider (`ONLINE_CAP_LDN`); no provider: scans empty, connect fails |
| `npns` | implemented | suspend state and receiver |
| `push` | implemented | |

Rows that stay documented defaults because they need something the host does not provide: BAAS / id tokens without a provider, the save data
transfer family, game card, rights ids, `GetBleScanFilterParameter` (2), ILogger 11, host add-on content setup, `nvdrv` 13 (no public description).
Online accounts, friends, rooms and cloud saves are described in the [Napple Online guide](../guides/napple-online.md).

## Save data

`fsp-srv`, `runtime/src/services/fs.c`. Horizon save data is transactional and the host mirrors that, so a quit or crash in the middle of a save
cannot corrupt it.

- `OpenSaveDataFileSystem` maps `<save-root>/<titleid>/save/space<N>/<type>[/<user>]` (the *live* directory the game writes into). For types
  account, device, system and cache a sibling `<live>.committed` holds the last committed snapshot and `<live>.napple-journal` marks the root as
  transactional. A 17th transactional root is logged (it is not transactional).
- **Commit** (`IFileSystem::Commit`, `nn::fs::CommitSaveData`): `tmp := clone(live)` (APFS `clonefile`, recursive copy elsewhere), `committed ->
  .previous`, `tmp -> committed`, remove `.previous`, `sync()`. A crash at any step leaves either the old or the new snapshot. Commit clears the
  dirty flag before the snapshot under the journal lock, so a write racing with the commit is not lost from the report, and a failed commit keeps
  it. A failed rename of the new snapshot also removes the temporary.
- **First open in a process** (recovery): finish an interrupted swap (`.previous` back to `committed` when `committed` is missing), delete stale
  temporaries, then `live := clone(committed)`, built in `<live>.restoring` first so a failed restore never destroys the live directory.
  Uncommitted work of a previous run is discarded, like on the console. A directory without marker and snapshot is a pre-journal save: it becomes
  the first committed state. Marker without snapshot = nothing was ever committed: empty.
- Open file descriptors keep pointing at the live directory (it is never swapped), so a game that keeps a save file open across a commit keeps
  working; writes after the commit are uncommitted until the next commit.
- When the process stops it reports which roots still had uncommitted changes (`fs: save ...: changes since the last commit are discarded`).
- Management commands: 21 / 25 / 28 Delete (by id, by space and id, by attribute), 22 / 23 / 35 CreateSaveDataFileSystem (plain / by system save
  id / with hash salt), 32 ExtendSaveDataFileSystem (sizes only grow), 33 DeleteCacheStorage, 34 GetCacheStorageSize, 52
  OpenSaveDataFileSystemBySystemSaveDataId, 53 OpenReadOnlySaveDataFileSystem (read-only; TargetNotFound for a save that does not exist), 57 / 58
  ReadSaveDataFileSystemExtraData(BySaveDataSpaceId), 59 / 66 WriteSaveDataFileSystemExtraData(WithMask), 60 / 61 / 62 OpenSaveDataInfoReader (all /
  by space / cache only; ISaveDataInfoReader::ReadSaveDataInfo), 69 / 70 / 71 extra data by attribute (read, masked write, masked read).
- Each save keeps its extra data (0x200 bytes) and id in `<root>.napple-meta`. Ids come from the root path: stable across runs but not the
  console's. A save opened before it was created gets an implicit record, and a later Create fills it in and succeeds; Create on a real record
  answers PathAlreadyExists. Deleting a save resets its journal, so a recreated root starts fresh. CleanDirectoryRecursively marks the root dirty.
- GetTotalSpaceSize / GetFreeSpaceSize of a save created with a size: that size, and size minus 16 KiB blocks in use; other file systems report
  1 GiB. Quotas are reported, not enforced on writes.
- Temporary storage is emptied at its first mount in a process; bcat storage is a plain directory.
- Argument order: commands taking (space, attribute) accept either order (space-first with zero padding, or attribute-first), since SDK versions
  differ.
- `NAPPLE_FS_TRACE=1` logs CreateFile / DeleteFile / Rename / Commit with the host path.

Online cloud saves build on the committed snapshots: see [cloud saves](../guides/cloud-saves.md).

## Loader

- `ldr:ro` / `ro:1` (`ro.c`, checks in `nro.c`): **partial**. RegisterProcessHandle, RegisterModuleInfo / RegisterProcessModuleInfo,
  UnregisterModuleInfo, MapManualLoadModuleMemory, UnmapManualLoadModuleMemory, with the ro module's checks and results. NRR checks: magic, size,
  program id and mask, kind (User for `ldr:ro`, JitPlugin for `ro:1`), hash list bounds; signatures are not verified. NRO checks: header, sizes as
  passed, page-aligned contiguous sections, sha256 listed in a registered NRR, not already loaded.
- What gets mapped is the image `napple build` rewrote ahead of time from every `*.nro` in the RomFS (`<data-dir>/nro/<module id>.{patched.bin,tables}`,
  manifest key `nro`), found by module id. It is relocated like the static modules (host overrides first, then every loaded module, NROs included)
  and its gates join the dispatch index. An NRO without such an image stops the runtime (`NAPPLE_IPC_LENIENT=1`: InvalidNro). Closing the session
  unloads its modules.
- The guest's own nn::ro relocation pass then runs over the mapped image. RELA entries are idempotent, but an import the runtime bound to a host
  override may be rebound by that pass to the guest's original function (the SDK's behaviour here is unchecked). The guest's bss buffer is unused
  (bss is part of the image). Like the static modules, an NRO whose text end and data start share a 16 KiB page fails to seal ("text overlaps data").
- Up to 128 modules; slots of unloaded NROs are reused, and the gate index is rebuilt and published atomically on load / unload.

## nvdrv

`nvdrv`, `nvdrv:a/s/t` (`nvdrv.c`). Layouts and their public sources: [nvdrv layouts](../reference/nvdrv-layouts.md). Every device or ioctl that is
not handled is refused (NotImplemented) and listed in `missing_ipc` as `nvdrv:ioctl <path> <cmd>`. The first Open of each device path is written
to the run summary.

| Device | Status | What it does |
|---|---|---|
| `/dev/nvmap` | partial | create / alloc / free / param / get_id / from_id as bookkeeping on guest memory |
| `/dev/nvhost-ctrl` | partial | syncpoints, events |
| `/dev/nvhost-nvdec`, `-vic`, `-nvjpg`, `-msenc` (host1x classes 0xF0 / 0x5D / 0xC0 / 0x21) | partial | SUBMIT command buffers run as host1x streams on register-level engines ([Media](#media)); submits complete at once and syncpoints advance |
| `/dev/nvhost-gpu`, `/dev/nvhost-as-gpu`, `/dev/nvhost-ctrl-gpu` | partial | state-tracked nodes (below); no channel executes, NVN is HLE |
| `/dev/nvdisp-disp0`, `-disp1` | partial | query surface only, **proposed** ABI (no public nvdisp ABI): type `'D'`, nr 1 GET_MODE (1280x720@60 handheld), 2 GET_VBLANK, 3 WAIT_VBLANK (never blocks, strictly increasing), 4 GET_SYNCPT (26 / 27), 5 GET_STATUS |

Command rows: QueryEvent, MapSharedMemory, GetStatus, SetAruid, DumpGraphicsMemoryInfo (verbose log of the nvmap totals) and InitializeDevtools
are implemented answers; `Unknown13` stays a stub (no public description).

**Multimedia channel ioctls** (type 0 unless noted): 'H' 0x01 SET_NVMAP_FD, 'H' 0x03 SET_TIMEOUT, 0x01 SUBMIT (command buffers must lie in live
nvmap handles; syncpoint increments are 0x14 bytes and land at once; fences are u32 thresholds), 0x02 GET_SYNCPOINT (allocated per channel and
index from id 32 up, released on close), 0x03 GET_WAITBASE (0), 0x07 SET_SUBMIT_TIMEOUT, 0x09 MAP_CMD_BUFFER / 0x0A UNMAP_CMD_BUFFER (one stable
32-bit device address per nvmap handle). Bad sizes, unknown handles and out-of-range indexes answer BadParameter. SUBMIT reads each command buffer
whose nvmap handle has a mapped address (`guest_ptr`), applies relocations (word = (target iova + offset) >> shift) to a copy and runs it on the
channel's engine under the nvdrv lock; handles without an address are accepted and nothing runs. Surfaces are addressed by device address >> 8
(MAP_CMD_BUFFER iovas).

**GPU nodes** (`nvdrv_nodes_core.c/.h`, routed by `gpu_node_ioctl`, which checks the type byte 'H' / 'A' / 'G' against the node kind). They answer
with consistent state, ordering errors and sensible values. Layouts are **proposed** (public nvgpu uapi / switchbrew, unverified against traces).

| Node | Ioctls (type, nr) | State kept |
|---|---|---|
| `/dev/nvhost-gpu` | 'H' 1 SET_NVMAP_FD, 3 SET_TIMEOUT, 8 SUBMIT_GPFIFO, 9 ALLOC_OBJ_CTX, 0xB ZCULL_BIND, 0xC SET_ERROR_NOTIFIER, 0xD SET_PRIORITY (50/100/150), 0x14 / 0x15 SET / GET_USER_DATA, 0x17 GET_ERROR_NOTIFICATION, 0x1A ALLOC_GPFIFO_EX2, 0x1D EVENT_ID_CTRL | bound address space, gpfifo ring, object classes (one each of the six advertised classes), fence = a dynamic syncpoint owned by the fd (SUBMIT with FENCE_GET bumps it, visible through nvhost-ctrl SYNCPT_READ) |
| `/dev/nvhost-as-gpu` | 'A' 9 INITIALIZE_EX, 1 BIND_CHANNEL, 8 GET_VA_REGIONS, 2 ALLOC_SPACE, 3 FREE_SPACE, 4 MAP_BUFFER_EX, 5 UNMAP_BUFFER | VA range (default 0x4000000..128 GiB, split 4 GiB, 64K big pages), reservations, mappings of live nvmap handles (fixed offsets only inside a reservation, no overlaps) |
| `/dev/nvhost-ctrl-gpu` | 'G' 1 ZCULL_GET_CTX_SIZE, 2 ZCULL_GET_INFO, 3 ZBC_SET_TABLE, 4 ZBC_QUERY_TABLE, 5 GET_CHARACTERISTICS, 6 GET_TPC_MASKS, 7 FLUSH_L2, 0xC GET_TPC_EXCEPTION_EN_STATUS, 0xD NUM_VSMS, 0x14 GET_GPU_TIME | ZBC colour / depth tables (16 each, identical entries shared), L2 flush count, monotonic GPU clock |

Ordering: ALLOC_GPFIFO_EX2 needs a bound address space (InvalidState); ALLOC_OBJ_CTX / ZCULL_BIND / SUBMIT_GPFIFO need the gpfifo; closing an
address space unbinds its channels; closing a channel frees its fence syncpoint.

## Media

### Host movie decoder (`runtime/src/movie.c`)

The SDK media library (`movie::Decoder`, Android MediaCodec over NvMM/NVDEC) is replaced by host decoders: the constructor and the 17 virtual
methods are bound by symbol name through `host_symbols_register`, so the library's own vtable slots land on the host code. Decoder types the host
does not take are forwarded to the library's original code.

- **VP9** on libvpx. Mode 1 (buffer) returns NV12 in the client's Buffer; mode 2 (NVN texture) converts to RGBA and writes into the game's
  NVNtexture through `gfx_texture_write_rgba` (block-linear swizzle, dirty tracking). Adapted from MacNativeGames (MIT), see THIRD_PARTY_NOTICES.md.
- **H.264** (`video/avc`) through VideoToolbox (`src/vt_h264.c`): Annex B or AVCC access units, SPS/PPS in-band (or csd via `vth264_config`),
  packed NV12 output into the same slots and pacing as VP9, B-frame reordering by pts (depth 2; 0 for baseline). The DecoderType value for AVC is 1,
  read from the movie library; `NAPPLE_MOVIE_H264_TYPE` overrides. `vt_h264.c` also has `jpeg_decode_rgba` / `jpeg_encode_rgba` (ImageIO).
- **Vorbis** (type 5, PCM, buffer mode; `movie_audio_core.[ch]`, `third_party/stb_vorbis.c`): raw packets wrapped into Ogg pages for stb_vorbis
  push-data. Headers in band, or out of band in Configure (`vorbispcm_set_headers_csd`): the format's `csd-0..2` buffers as one Xiph bundle, three
  single packets, or identification + setup (the empty comment header is synthesised, Android MediaCodec layout). A rejected header leaves the
  collection untouched; audio packets before the headers are dropped with one log line. On by default (`MOVIE_VORBIS_DEFAULT 1`);
  `NAPPLE_MOVIE_VORBIS=0` turns it off.
- **Opus** decodes in the core (libopus, `-DNAPPLE_HAVE_OPUS`) but the movie library has no Opus decoder type (DecoderTypeToMime /
  IsDecoderTypeSupported cover 1..5), so it stays core-only; `hwopus` is separate.
- `NAPPLE_MOVIE_HOST=0` leaves the SDK's own decoder in place.

### host1x engines

Portable cores (`runtime/src/services/*_core.c`, no macOS headers).

| Piece | Files | What it does |
|---|---|---|
| host1x stream | `nvdrv_media_core.[ch]`, `nvdrv_media_regs.h` | parses command buffers: SETCL (with register mask), INCR, NONINCR, MASK, IMM, RESTART, GATHER (command gather and register gather, depth 4), EXTEND; registers arrive as THI method / data pairs (`INCR 0x10 count 2`); EXECUTE (offset 0xC0) runs the engine. A malformed op or unmapped gather fails the SUBMIT (BadParameter); a failed decode is counted and written to the status structure. Writes to registers outside the map are counted (`MediaStats.unknown_regs`, THI word 0 INCR_SYNCPT excluded) and summarised on close. `nvdrv_media_regs.h` is the only place register offsets live |
| NVDEC (0xF0) | same | application id = codec (H.264 3, VP8 5, H.265 7, VP9 9); picture-info, bitstream and output surfaces; DPB output slots are registers 0x10c+idx (luma) / 0x11d+idx (chroma), the output slot is `curr_pic_idx` of the H.264 picture-info structure (0x103 is a frame counter); execute -> `DecoderBackend.decode`; status structure (error, width, height, macroblocks). The H.264 sequence is described by the picture-info structure only (slice NAL units carry no SPS/PPS), so `nvdec_synth_h264` writes the SPS/PPS from it (word map in `nvdrv_media_regs.h`; the synthesized SPS claims 15 reference frames, since 16 makes VideoToolbox evict a live reference). Parameter sets found in the bitstream and VP9 key-frame configs reach `configure()` once per change (reset first; `MediaStats.configures`). NVDEC records the pitch it wrote each output surface with; H.264 pitches come from picture-info words 35 / 36, VP9 from words 20 / 21 (`NVDEC_VP9_W_PITCH_*`, **proposed**, ignored unless >= the key frame width, multiple of 16, <= 0x10000) |
| VIC (0x5D) | same | slot-0 input luma / chroma 0x100 / 0x101 (read back with the pitch NVDEC recorded, else the old 256-aligned / packed guess), 0x1c1 control word, 0x1c2 config structure (output surface word at +0x20), output luma / chroma 0x1c8 / 0x1c9; NV12 -> RGBA8 / BGRA8 / RGBX8 / NV12 (BT.601 limited), pitch- or block-linear output; NV12 copy when no config is readable. `NAPPLE_VIC_DUMP` dumps output frames |
| NVJPG (0xC0) | same + `jpeg_core.[ch]` | baseline JPEG (all samplings, restart intervals, grey) to RGBA or NV12, optional scaling and pitch, status (error, size). Real `nvjpg_drv_pic_param_s` (0x1240 B); decodes a JPEG file in the bitstream buffer or, with no SOI, the header-less entropy segment from the structure's decoder tables (`napjpeg_decode_raw_rgba`), pixel-identical. Encode: pic-setup structure of 12 words with word 6 = 1: RGBA8 in (pitch word 9), JPEG out, quality, 4:2:0 flag; status error / width / height / bytes |
| MSENC (0x21) | same | `EncoderBackend`: reduced setup structure at 0x101 (width, height, pitches, bitrate, fps, flags, out capacity), NV12 input 0x102 / 0x103, output 0x104, status 0x109 (error, bytes, IDR flag, frame count); registers **proposed** (the real NVENC method map is unknown) |
| Codec headers | `codec_headers_core.[ch]` | H.264 SPS (incl. cropping, high-profile scaling lists), VP9 uncompressed header, VP8 frame tag, H.265 SPS skeleton (size only); writers `h264_build_sps/pps` |
| JPEG caps | `caps_core.[ch]`, `caps.c`, `jpeg_core` | the caps services above |

### Decoder and encoder backends

- `DecoderBackend` ABI 1 (`decoder_backend.h`, frozen): `open/close/decode` plus optional `reset/configure`; the `abi` field is checked by
  `decoder_backend_install` (-1 on mismatch); call order and the synchronous in-decode-order rule are in the header comment. Frames in (codec,
  picture-info, bitstream, output planes, the guest's row pitches in `DecodeRequest`), NV12 out.
- Null backend (`decoder_null_core.c`): size from the codec headers, a deterministic grey picture. `NAPPLE_DECODER=null` selects it.
- VideoToolbox backend (`services/decoder_vt.c`, installed from a constructor, never compiled on host / Linux builds): H.264 through `vt_h264.c`.
  `decode()` is synchronous: each request is decoded and flushed, so the frame is in the guest surface when it returns. VP9 is refused (open returns
  NULL, status error 1) unless `NAPPLE_VT_VP9=1` and VideoToolbox reports a VP9 decoder (vpcC from `configure`, superframes split with
  `vp9_superframe_split`; this path has not been exercised).
- `EncoderBackend`: null encoder (fake Annex B) or VideoToolbox H.264 (`decoder_vt.c`); `NAPPLE_ENCODER=null` keeps the null one.
- `media_write_prepare` (set by `media_gfx_glue.c`) tells gfx before the host writes a surface.

Known limits: NVDEC output is pitch-linear NV12 (the hardware writes block-linear); decode runs inside the nvdrv lock. The H.264 NVDEC + VIC NV12
path is checked against real SDK streams; VP9 through NVDEC, NVJPG, MSENC and nvdisp are tested with synthetic streams only.

## Library applets and the am interfaces

`applet.c` (appletOE: ICommonStateGetter, ISelfController, IWindowController, IAudioController, IDisplayController, ILibraryAppletCreator,
ILibraryAppletAccessor, IStorageAccessor, IApplicationFunctions, IDebugFunctions), `applet_ae.c` (appletAE side used by firmware applets), answers in
`applet_lib_core.[ch]`.

**State rows.** A focus message is queued at launch. The guest's settings are stored and read back: master volumes, VR mode, auto sleep and its
override, idle detection, screenshot permission, CPU boost, home-button blocking, sleep lock + event, fatal section, window visibility, foreground
rights, GPU time slice, HDCP, GPU-error abort delay, system button, Home short press, PrepareForJit. Illuminance: `NAPPLE_ILLUMINANCE=<lux>` makes
the sensor present. Capture image slots read the graphics back end's last completed frame (`applet_capture_host.c`; `applet_capture_set_source`,
none = opaque black). Host events: `applet_signal_gpu_error`, `applet_signal_friend_invitation`, `applet_display_resolution_changed`.

**Notable rows.**

| Interface | Rows |
|---|---|
| IApplicationFunctions | 34 / 36 full-license answers; 70 / 71 / 310 ExitRequested message, then a clean stop after a 3 s grace (`NAPPLE_EXIT_GRACE_MS`); 100-102 copyright; 110 / 111 play statistics of the running title; 120 ExecuteProgram and CreateApplication*AndRequestToStart restart the running program (macOS `execv`, user channel carried over); 123 GetPreviousProgramIndex (-1); 130 GetGpuErrorDetectedSystemEvent; 140 GetFriendInvitationStorageChannelEvent; 180 / 181 launch-required version (persisted per title, only raised); PopLaunchParameter, EnsureSaveData (offline answers); GetSaveDataSize reads `UserAccountSaveDataSize` / `JournalSize` from `control.nacp` (32 MiB each without it) |
| ISelfController | 120 SaveCurrentScreenshot (current frame into the caps album; `NAPPLE_DEBUG_SAVE_SCREENSHOT` triggers it); SetLcdBacklighOffEnabled drives lbl's switch state |
| IDisplayController | 28 TakeScreenShotOfOwnLayerEx |
| ICommonStateGetter | 900 SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled (no-op); 1004 / 1005 panel 137 x 77 mm, 1280 x 720 |
| ILibraryAppletAccessor | 60 PresetLibraryAppletGpuTimeSliceZero |
| system buffer sharing | IsSystemBufferSharingEnabled answers the failing result of an unprivileged process; GetSystemSharedLayerHandle (41, 42) fails with 0x1480 |

Requests one runtime cannot satisfy (another application or program, the movie maker, child applets) answer the am result 0x1480; empty storage
channels answer 0x680; preference setters are no-ops (`IPC_STUB`). NotifyMessageToHomeMenuForDebug answers like a console with no Home menu
process.

**Handler registry.** `libapplet_register_handler(id, handler)` (`applet_lib_core.h`) answers a library applet id before any built-in answer; the
handler sees the pushed and interactive storages and returns output storages on both channels.

**Firmware applets.** `libapplet_program_for(id)` maps applet ids 0x0A-0x1A to the firmware program ids (`0100000000001001`...). A firmware applet
runs through the host (child process or in process) when `libapplet_firmware_enabled` says so: the Mii Maker (0x12) by default, any other entry
with `NAPPLE_FIRMWARE_APPLET_IDS=0x11,controller,...` or `NAPPLE_FIRMWARE_APPLETS=all`; `NAPPLE_FIRMWARE_APPLETS=0` turns all off. Without the
firmware the built-in answer is used. Interactive channel: `PushInteractiveInData` / `PopInteractiveOutData` / `GetPopInteractiveOutDataEvent` are
real; a firmware applet gets `iin<N>.bin` and answers `iout<N>.bin` (`PopInteractiveInData` / `PushInteractiveOutData` of `applet_ae.c`); output
storages are a queue (`out0..out7`); `GetResult` returns the applet's exit result (`result.txt`).

**Built-in applets.**

| Applet (id) | Answer |
|---|---|
| software keyboard | foreground and inline (background mode replies over the interactive channel until Finalize). The KeyboardConfig header / sub / guide texts, initial text, length limits and invalid-character flags are parsed (`AppletSwkbdConfig`, `applet_utf16le_to_utf8`) and passed to `frontend_text_input`; `NAPPLE_VERBOSE=1` dumps them. `NAPPLE_SWKBD_ANSWER` scripts the answer |
| controller support (0x0c) | `player_count_min..max` from the argument header (unset = 1..4); connected pads; single mode reports npad 0 as the selected id, otherwise `0xffffffff`; output `{s32 players; u32 selected_id; u32 result = 0}` (0xC). `NAPPLE_CONTROLLER_SUPPORT=cancel|strict` |
| player select (0x10) | profile list from `applet_set_profile_lister`; output `{u64 result; u8 uid[16]}` (0x18), result 2 = cancel. `NAPPLE_PLAYER_SELECT=<index>|cancel` |
| error (0x0e) | dismissed (success, no output) unless the frontend hook (`applet_set_error_hook`) shows it; header type, jump flag and first argument word are logged and put in the run summary (`summary_note`); a jump-to-settings request is not followed |
| network connection (0x0f) | see below |
| web / offline web (0x13, 0x17) | `applet_web_core.c` parses the WebArgs TLV storage and answers `WebCommonReturnValue` (exit reason + last URL); see below |
| photo viewer / album (0x15) | `applet_album_core.c` lists the caps album (`applet_album_set_source`), shows decoded images through `applet_ui_show_image`, and in select mode returns the picked entry |

Web and album register through `libapplet_register_handler` (`applet_web_register`, `applet_album_register`). Struct layouts follow libnx and are
[unverified] where the source says so.

**Offline web documents.** An OfflineHtmlPage path is relative to `html-document/` of the HtmlDocument RomFS (`<data-dir>/html/html-document/<name>.htdocs/html/<RegionLang>/...`),
also settable with `applet_web_set_html_root` / `NAPPLE_HTML_ROOT`, and reaches the frontend as a `file://` URL through `applet_ui_open_web`; with
no hook the user leaves at once on the requested page. Resolution decodes percent escapes, then rejects `..`, backslash and NUL, and refuses
symlinks leaving the `<data-dir>/html` tree. Legal information: `napple build` writes a title's LegalInformation content (CNMT type 5) to
`<data-dir>/legal/` (`NAPPLE_LEGAL_ROOT` overrides). System data documents come from the firmware archive `<data-dir>/../sysdata/<id>.bin`, extracted
once by `web_romfs_extract`. `web_manual_path` / `applet_web_open_manual` open a title's manual the way the HOME menu does. Triggers for tests:
`NAPPLE_OPEN_MANUAL=<secs>[:<RegionLang>]`, `NAPPLE_OPEN_WEB_DOC=<secs>:<kind>:<id>:<path>`.

### Network connection applet (0x0f)

- nifm IRequest **Cancel** releases and resets only an accepted request. A rejected one keeps its result, because the SDK cancels before
  HandleNetworkRequestResult asks GetAppletInfo.
- **GetAppletInfo** for a failed request names the netConnect applet (id 0x0f, mode 0) and lists what it reads, since the SDK pushes only the
  listed storages: CommonArguments, then `{u32 0; Result; Uuid}` (0x18 bytes), then -1.
- **Output:** one storage `{u32 0; Result}`, as netConnect writes it. The Result is 2110-0190 (ErrorHandlingCompleted; the title resubmits) when a
  connection was made, and 2110-2900 when the user left without one. Offline, the host answers 2900. (An output of 0 would be turned into 0190 by
  the SDK and make the title retry.)

### Frontend hooks

The controller (0x0c) and player select (0x10) applets ask the frontend through `applet_controller_ask` / `applet_player_select_ask` (hooks
`applet_set_controller_ui`, `applet_set_player_select_ui`; -2 = no UI, keep the headless answer), the error applet through
`applet_set_error_hook`, web through `applet_ui_open_web` and the album through `applet_ui_show_image` (answer 1 select, 0 next, -1 close;
`applet_ui_album_selecting()` says whether the viewer may select). The Mac frontend registers sheets for all of them
(`frontend/src/applet_sheets.m`, `register_applet_sheets` in `app.m`). Scripted answers win over any sheet: `NAPPLE_CONTROLLER_SUPPORT`,
`NAPPLE_PLAYER_SELECT`, `NAPPLE_WEB_ANSWER` (back | closed | requested | error | n), `NAPPLE_ALBUM_PICK` (index | first | next | none),
`NAPPLE_SWKBD_ANSWER`.

## Frame pacing and timing

All guest time derives from one host clock: `GetSystemTick` and `cntpct_el0` are `mach_absolute_time` scaled to 19.2 MHz (`runtime_guest_ticks`,
factor 12/625 on nanoseconds), `cntfrq_el0` reads 19200000, and the `time:u` steady clock uses the same ticks, so every guest-visible clock agrees.
The only periodic source is the vsync event of `vi:u` (`vi.c`): a thread with absolute deadlines (`next += period`, no drift; a stall longer than 8
periods resynchronises instead of bursting).

Three rates are kept apart, and only the first two can change what the game feels like:

1. **Game clock / guest cadence** (`timescale.c`). The virtual vsync fires every `TS_GAME_VSYNC_NS` (1/60 s) divided by the Speed control and
   nothing else (`timescale_vsync_period_ns`); every guest clock follows the same scale. A title at swap interval 2 waits two vsyncs (30 Hz game
   logic). The deadline stepper `timescale_deadline_step` is shared by `vi.c` and the tests.
2. **Speed** (`speed_percent`, turbo, slow motion, `speed_mode`): the only control that makes the game faster or slower.
3. **Present rate** (`fps_mode`): a host property, `timescale_set_fps`. 0 game pacing, 1 match the display refresh (ProMotion 120 Hz), 2 custom N
   (`fps_cap`, 15..999), 3 uncapped (`CAMetalLayer.displaySyncEnabled = NO`). It decides which finished game frames the presenter shows and is never
   read by the guest clocks. Above the game's frame rate the display repeats the last image (frame generation, `mtl_interp.m`, can fill
   in-between images); real extra game frames need a per-title timestep patch. Below it (custom 30) the presenter drops frames. `vsync` off lets
   the layer present as soon as an image is ready (tearing possible); it never touches game speed either. `gfx_vsync_period_ns()` is always the
   game's 16.67 ms. In `mtl_present.m` the guest-visible frame time is `swap interval / 60 / speed` in every mode; the headless path (no layer)
   therefore always runs the game at its real speed whatever `fps_mode` says.

Settings: `settings.json` has `fps_mode` (+ `fps_cap` as the custom rate). Older files that stored `fps_cap` as a rate the guest was paced by
migrate on load (0 -> game pacing, 1000 -> uncapped, N -> custom N and, when the file has no `speed_percent`, Speed = N/60 so the old feel is kept
explicitly). Headless: `NAPPLE_FPS_MODE=0..3`, `NAPPLE_FPS_HZ=<N>` (mode 2); `NAPPLE_SPEED=<factor>` is the speed. `NAPPLE_FRAME_TRACE=<file.csv>`
writes `kind,real_ns,game_clock_ns` for every guest vsync (V) and presented frame (P); `tools/fps/frame_trace.py` runs the title x mode matrix and
asserts game clock 1.00 +-0.02 per wall second, guest vsync 60 Hz +-3 % and presents never above vsync. Unit test: `runtime/tests/test_timescale.c`
(10 simulated seconds at present rates 30/60/144/999/display/uncapped: identical vsync count and game clock, +-0.1 %). The statistics overlay (F9)
shows `game` fps (guest frames) and `shown` fps (presented images) separately.

## Shared fonts (`pl:u`)

The system fonts are Nintendo data and are never shipped. Extract them once from the user's own firmware:
`napple build --dump-fonts <Firmware.zip|dir> -k <prod.keys> -o "~/Library/Application Support/Napple/system/fonts"` (system data titles
0100000000000810..0815; each .bfttf is decoded: every u32 XOR 0x06186249, then {magic 0x18029A7F, u32 big-endian size, TTF}). The service lays
the fonts out in a 0x1100000-byte shared memory as {0x18029A7F, size ^ 0x49621806, TTF} and reports the offset of the TTF itself. Type mapping
(libnx PlSharedFontType): 0 std, 1 zh-CN, 2 zh-CN extension, 3 zh-TW, 4 Korean, 5 Nintendo extension, by file name. Without the fonts zero-size
fonts are served and titles render no text.

## Per-game data (`game.json`)

`game.json` in the data directory: `{"title_name":..., "nvn_major":..., "nvn_minor":...}`. A title checks the NVN API version
(`nvnDeviceGetInteger(API_MAJOR/MINOR)`) against its build-time constant and silently skips graphics initialisation when it does not match, so the
runtime passes the title's version to gfx (0,0 = 55.15). See [Graphics](gfx.md).

## Mods runtime

`fs.c` `storage_with_overlay` replaces the RomFS IStorage by a virtual image (`romfs_map.c` `romfs_merge_open`: original tables +
`<data-dir>/mods/overlay.json`; file data stays an extent of the original or of the mod file). `mods_runtime.c` loads `mods/cheats.txt` and
`mods/config.json`; `mods_cheats.c` is the cheat VM, run once per frame from the `vi` vsync thread; `app.m` applies the mod config to a copy of
the settings. `NAPPLE_NO_MODS=1` turns it all off. Format and authoring: [mods](../mods/README.md).

## IPC reference lists

`runtime/tests/ipc_reference/<interface>.txt` lists, per interface, the command ids a title can send: `id<TAB>name<TAB>min_fw<TAB>y|n`, first
line `# interface <name> (<source>)`, where `<name>` is the interface string the runtime gives the object (the first argument of `IPC_OBJECT`, e.g.
`hid`, `IAudioDevice`, `audren:u`, matched exactly); `# alias <iface>` and `# table <file.c>:<table>` comment directives help when the binding cannot
be read from the source. `y` = reachable by an application on firmware up to 22.x (removed commands stay `y`); `n` = system / debug only, Switch 2,
23.0.0+, system-applet services or unnamed. Lists come from switchbrew / libnx (ids, names, firmware only).

`runtime/tests/test_ipc_reference.c` (part of `make test` and `make test-host`; `make test-ipc-reference` for detail; `-v` also lists rows absent
from the reference and name differences) reads the command tables from the sources and fails when an app-reachable id has no row. Covered
interfaces include hid, IAppletResource, IActiveVibrationDeviceList, hidbus, irs, the audio interfaces, psm, apm, fgm (`fgm::IRequest`, distinct
from nifm's IRequest), lbl, spsm, set, set:sys, set:fd, btm:u / IBtmUserCore, and the application-facing am interfaces (`appletOE`,
`IApplicationProxy`, `ICommonStateGetter`, `ISelfController`, `IWindowController`, `IAudioController`, `IDisplayController`,
`ILibraryAppletCreator`, `ILibraryAppletAccessor`, `IStorageAccessor`, `IApplicationFunctions`, `IDebugFunctions`; the am `IStorage` shares its name
with the fs fixture).

`tests/linux/known_calls_titles.txt` lists the commands the reference titles call; `check_known_calls.py` fails when one disappears or becomes a
stub. Current coverage numbers are in [feature support](../reference/feature-support.md).

## Debug aids

| Variable / action | Effect |
|---|---|
| `NAPPLE_INPUT_SCRIPT="10:A,12:PLUS:0.3,30:L+R:0.8"` | scripted key presses (seconds after HID is configured : button [: hold]); `+` joins a chord pressed atomically; uses the bindings of input.json; times and holds may be frames (`f600:A:f10`, or `NAPPLE_INPUT_SCRIPT_FRAMES=1`) |
| `NAPPLE_INPUT_LOG=1` | logs every change of player 1's merged state |
| `kill -USR1 <pid>` | dumps every guest thread (state, wait, backtrace) without stopping |
| `NAPPLE_OP_MODE=docked|handheld` | operation mode |
| `NAPPLE_AUDIO_VOLUME=0.05`, `NAPPLE_AUDIO_DEVICE=1` | audible tests at low volume |
| `NAPPLE_FE_OVERLAY=<tab>`, `NAPPLE_FE_VIEWSHOT=<png>:<secs>` | open the overlay at start; render it to a PNG |
| `tools/dev/synth_keys.m` | posts real CGEvents |
| `NAPPLE_SCREENSHOT=<prefix>`, `NAPPLE_SCREENSHOT_FRAMES=1,60,...` | PNG of presented frames |
| `NAPPLE_WATCH=main+0x<offset>` | logs every guest write to a location |
| `NAPPLE_PEEK`, `NAPPLE_WAIT_SECS` | memory peek, wait diagnostics |
| `NAPPLE_FS_TRACE=1` | file system trace |
| `NAPPLE_MOVIE_HOST=0` | keep the SDK movie decoder |
| `NAPPLE_VERBOSE=1` (`-v`) | verbose log, including the `nvdrv` data lines and the media register summary |

The full list is in [environment variables](../reference/environment-variables.md).

## Tests

- `make -C runtime test` (macOS): the service and kernel unit tests, including `test_fs_journal.c` (commit, rollback, interrupted commit, nested
  directories, adoption of old saves, open fds), `test_timescale.c`, `test_ipc_reference.c`, `test_svc_*` (e.g. `test_svc_fs`, `test_svc_applet`,
  `test_svc_bcat`, `test_svc_nfp`, `test_svc_amiibo`, `test_svc_caps_e2e`, `test_svc_movie_audio`, `test_svc_nvdrv_media`).
- `make -C runtime test-host` (any POSIX host): the portable kernel, loader and service code with `tests/host` stand-ins: `test_kernel_ipc`,
  `test_loader`, `test_nvdrv`, `test_fs_savedata`, `test_fs_journal`.
- Linux harness (`tools/linux-check/`, IPC mock): `runtime/tests/linux/run_applet.sh` (`test_svc_applet`, `_libs`, `_host`, `_ae`, with the web /
  album / caps / jpeg cores), `runtime/tests/linux/run.sh nvdrv_nodes`.
- `make -C audio test`, `make -C input test` for the library sides.

History of these subsystems: [change archive](../archive/changes/README.md).
