# Services: 24 new services and deeper commands (UNBUILT on macOS)

Branch `services-2026-10-24`, based on `1f4b902`. Scope: `runtime/src/services/` except `nvdrv.c` and `fs.c`, plus tests and tooling. No kernel code, no
`gfx/`, no `audio/`, not pushed.

## Read this first
**None of this has been built or run on macOS.** `runtime/` does not build on the Linux box this was written on (Mach and CommonCrypto headers, AArch64
assembly, Cocoa/Metal links). What was done instead:
- the portable logic lives in `*_core.c` files with no macOS dependency and is unit-tested on Linux (`tools/linux-check/run-tests.sh`, 24 test binaries,
  all passing, also under `-fsanitize=address,undefined`);
- the IPC tables (thin glue) were run on Linux against `tools/linux-check/ipc_mock.c`, a mock that copies the real framing helpers but is **not** the real
  IPC layer, `handle_*` or event implementation;
- every `runtime/src/services/*.c` passes `gcc` and `clang` `-fsyntax-only` with stub headers (`tools/linux-check/check-syntax.sh`); `fs.c` and `nvdrv.c`
  are skipped (not mine), and `hid.c` tolerates only its pre-existing Darwin `ucontext` code;
- `hid.c`'s IPC handlers and `tests/test_hid_depth.c` could not be compiled or run at all on Linux (Mach + input library): syntax-checked only.

**Unverified against a console or a real title:** every command id, argument layout, struct layout and result description below is the author's recollection of
the public documentation. Result descriptions that are guesses are marked `[unverified]` in `src/services/hz_result.h`. The BSD errno / netdb numbers
(FreeBSD numbering) and the ClockSnapshot/CharInfo layouts are the ones the author is most sure of. An id that is wrong makes a command fail loudly through
the generic unknown-command policy, which is the intended failure mode; a wrong layout is silent and is what the verification must look for.
Services are reachable only if the game's NPDM lists them, so a new service may never be hit by LAS or SMO.

Verification plan for the Mac: `docs/changes/2026-10-24-services-VERIFY.md`.

## Status of every service
real = state/logic behind the answer and unit-tested; partial = some commands real, data source or the rest default; stub = safe offline default.

| Service | Status | What is real | What is not | Commit |
|---|---|---|---|---|
| `psm` | partial | percentage, charger type, voltage state, enough-power, charging flags, session events (never fire) | values are configured (`NAPPLE_BATTERY`, docked/handheld), not the Mac's battery; voltage thresholds are host-chosen | 5ec1141 |
| `lbl` | real | brightness set/get/apply with clamp and NaN guard, switch, dimming, auto, VR brightness | nothing reaches the display; no light sensor; reflection/mapping commands accepted unread | 2f0be06 |
| `mii:u` | partial | local player's Mii + 6 defaults, CharInfo 0x58 / element 0x5c with size asserts and range validation, GetCount/Get/Get1/UpdateLatest/BuildRandom/BuildDefault/FindIndex/GetIndex | read-only; no StoreData (Get2/Get3/Convert), no editing; BuildRandom is a default of the requested gender; default Mii appearance is neutral, not Nintendo's six | 2bd6842 |
| `caps:su` | partial | save calls succeed with an album entry and no file; refused after `SetScreenShotPermission(Disable)` | which save command returns what is a guess; no file is written | b11c5dc, d8c3862 |
| `caps:u` | partial | empty album lists, not-found for load/size/delete, shim version | all ids from memory | b11c5dc |
| `ncm` | stub | empty meta databases and storages | everything | 42dc077 |
| `ns:am2`, `ns:ec` | partial | ApplicationLanguage <-> language code, language selection with fallbacks, free/total space via statvfs | no application records; control data (NACP) not found; e-commerce fails offline | 2f13805 |
| `ns:vm` | stub | - | always up to date | 2e08f54 |
| `sfdnsres` | partial | hstrerror / gai_strerror text | every lookup fails (TRY_AGAIN / EAI_AGAIN); tuple layouts from memory | 3a22a57 |
| `spsm` | stub | - | always awake | 76d6307 |
| `bcat:u` | partial | directory/file name validation, progress record, object graph | cache always empty; sync "finishes" at once with the offline result | f810849 |
| `ldn:u` | partial | initialize/AP/station/connect state machine with transition checks, per-object state, state-change event | no radio: scan empty, connect fails | 76336b7 |
| `irs` | stub | npad -> camera handle | processors report not connected; shared memory zeroed | 98329cb |
| `audin:u` | partial | device list, rate/channel negotiation, append/release/flush/contains queue, silence written into the guest buffer | no microphone; buffers complete instantly (no timing); the Mac mic is never opened | a3fa4e2 |
| `hidbus` | stub | bus-type validation | no device is ever found | e0e4690 |
| `pdm:qry` | partial | running title's play minutes and launch count | other titles / events empty | 2b9bb0f |
| `set:fd` | real | in-memory item store with limits, replace/reset, key iterator | not persisted; `set:sys` does not read it back | 1118976 |
| `olsc:u` | stub | - | command ids are guesses | d66030c |
| `ectx:aw` | stub | remembers the last result | command ids are guesses | cbab631 |
| `fgm` (+`fgm:0`, `fgm:9`) | stub | requests remember module and range | event never fires; the extra aliases were not asked for | eaa8a1b |
| `btm:u` | stub | - | scans empty, connect fails | deb3087 |
| `ovln:snd` | stub | counts notifications, keeps the id | shows nothing | 337734c |

## Command depth (existing services)
IPC_STUB rows before -> after, counted from the tables: `appletOE` 87 -> 55, `hid` 30 -> 21, `time` 12 -> 8, `offline.c` 146 -> 146, `misc.c` 17 -> 17.
Which stubs "real titles call" could not be observed (nothing was run); the rows converted are the ones that carry state the guest can read back.
- **appletOE** (8971b44): ~30 rows now store and return what the guest set (master volumes, VR mode, auto sleep, idle detection extension, screenshot
  permission, cpu boost, home-button blocking depth, recording state, crash report, ...). Requests shorter than the expected argument are ignored, not fatal.
  Still stubs: sleep-lock rows, resolution/launchable/GPU-error events, capture-image calls, `PopLaunchParameter`, `EnsureSaveData`, `GetSaveDataSize`,
  pseudo device id, library-applet stubs (RequestExit/Terminate/interactive data/info), debug functions.
- **hid** (44c9b1e): six-axis fusion and zero-drift per handle, player LED pattern, joy hold type, handheld activation mode, per-pad home-button protection,
  vibration permission (now actually silences rumble), communication mode, supported styles read back; adds 126/127/130/133-135/211/1000/1001.
  Still stubs: xpad, gesture, console six-axis, `IsSixAxisSensorAtRest` (always true), firmware/debug-pad.
- **time** (c1459e8): `LoadTimeZoneRule` (was an unknown command), rule-aware `ToCalendarTime`/`ToPosixTime` (100/201 used to ignore the rule), location
  list/count/version, `GetClockSnapshot` and `...FromSystemClockContext`, the two snapshot calculations, automatic correction as a real flag. Behaviour
  changes: device location is the host IANA zone (was always `UTC`); automatic correction and network accuracy now report true (host clock is NTP-accurate).
  Still stubs: Set* clock calls, steady-clock test/RTC rows, initial year, `ToCalendarTimeWithMyRule` stays host-local.
- **offline.c** (ced49f4): kept as offline stubs but with Horizon-style results. `bsd:u` has a descriptor table: `Socket` returns a descriptor (it used to
  always fail), unknown descriptors give EBADF, connect ENETDOWN, recv on an unconnected stream ENOTCONN, datagram recv EAGAIN, send without destination
  EDESTADDRREQ, IPv6 EAFNOSUPPORT, table full EMFILE. `nifm` `Submit` rejects at once and `GetResult` then returns the communication-disabled result (`Cancel`
  clears it). `ssl` `DoHandshake` fails. `friend` `Pop` on an empty queue returns an error instead of a zeroed record. `bsd` Poll/Select still return
  "timed out" at once without reading their arguments. The nifm/ssl/friend result descriptions are guesses.
- **misc.c** (aoc, pctl): unchanged; the answers (no add-on content, unrestricted device) are already the correct offline ones.

## Other changes
- `runtime/Makefile`: one appended line, `-include tests/services.mk`, which adds `make test-services` (builds and runs `tests/test_svc_*.c` and
  `test_hid_depth` against the real runtime objects). `make test` is unchanged. Never run.
- New headers/cores in `runtime/src/services/`: `hz_result.h`, `svc_common.h`, and `*_core.[ch]` for psm, lbl, mii, caps, ns, sfdnsres, bcat, ldn, irs, audin,
  hidbus, pdm, setfd, time, applet, hid, offline.
- `docs/runtime-services.md`: one section appended (no existing text edited), so the older `appletOE`/`time`/`hid` rows above it are stale.

## Not done
- No service was exercised by a game; no NPDM was inspected to see which of these LAS/SMO request.
- `set:sys` is not wired to `set:fd`; `psm` does not read the Mac battery; `audin` does not touch the Mac microphone (and `Info.plist` has no
  `NSMicrophoneUsageDescription`, see the 2026-10-23 note).
- `fs.c`, `nvdrv.c`, the kernel, `gfx/`, `audio/` untouched (another session owns them).

## Known smaller items
- Linux tests print two harmless warnings (`system()` result in test_svc_ns, a `snprintf` truncation hint in test_svc_setfd).
- `hid.c` has two pre-existing `-Wtautological-compare` warnings under gcc (lines moved by this branch, not introduced by it).
