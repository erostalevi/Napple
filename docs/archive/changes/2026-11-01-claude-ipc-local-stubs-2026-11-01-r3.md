# 2026-11-01 ipc-local-stubs (batch 2026-11-01-r3, mac)

Round 2 (ipc-depth-sys) gave every application-reachable command of the system services a row, ~100 of them documented-default `IPC_STUB`s of LOCAL services. This round turns **66** of them into real `IPC_IMPL` behaviour, each with a unit test, and gives IAudioOut 12/13 a real per-stream gain. Status words: **tested** = unit test passes here (with synthetic data, plus the real firmware NgWord archive for ngc), **verified** = a real title called it.

## New code
- `romfs_read.[ch]` (portable): parses a RomFS image's directory and file tables through a read callback (`romfs_img_open/lookup/child/read`; layout from libnx/switchbrew). Used by fs.c (the RomFS as a file system) and ngc.c (the firmware NgWord archive).
- `audio/`: `napple_audout_set_volume/get_volume`, a per-stream gain (0..1, clamped, NaN = 0, atomic) applied to the converted samples in `consume()` before they reach the engine mix. `audio/tests/test_audout.c` `t_volume`: unit gain leaves samples alone, 0.25 scales both channels, 0 is silence, clamps, release order and the played-sample count are untouched.

## Rows converted (66)
| Service | Rows | What they do now |
|---|---|---|
| fsp-srv (22) | 2, 7, 9, 10 | the RomFS (and `FileSystemProxyType` Control / Manual) as a read-only `IFileSystem`: `GetEntryType`, `OpenFile` (IFile extent of the same storage `IStorage` serves, so mods / delta layers / overlay agree), `OpenDirectory`; Rom and Data serve the program id's RomFS (the application, its update title, a DLC id, a `sysdata/` archive); Control = `control.nacp` of the data dir linked into `<save-root>/<title>/content/control`; Manual = `<data-dir>/html`; Code, Logo, Meta, Package: "path not found" |
| | 201, 205 | `OpenDataStorageByProgramId`, `OpenDataStorageWithProgramIndex` (index 0 = the running program) |
| | 64, 67, 68, 616 | save lookups: internal storage by (space, id); `FindSaveDataWithFilter` / `OpenSaveDataInfoReaderWithFilter` over the save meta records with the five `SaveDataFilter` flags; `GetSaveDataCommitId` (the extra data's commit id, now advanced by every commit) |
| | 612, 27, 620, 630, 631 | free space of the volume under the save dir; exFAT supported; SD encryption seed kept; SD accessibility flag + directory check |
| | 1004, 1011, 1016 | global access log mode (1005 reads it back), program index / version for the access log, flush (nothing is buffered) |
| | 100, 130 | image directory (NAND / SD) and custom storage (user / SD) as writable directories below the title's save root; other ids: 0x2EE202 |
| | 510, 511 | system data update notifier object (`GetEventHandle`), signalled by 511 |
| IFile 5, 6 / IStorage 5 (3) | `OperateRange` | QueryRange = all-zero 0x40-byte `QueryRangeInfo`, InvalidateCache ok, Clear zero-fills a writable host file (refused on read-only objects), unknown operation refused |
| acc:u0 (4) | 6, 52, 110, 111 | profile digest (hash of uid, creation time, nickname), TrySelectUserWithoutInteraction (the local user), save-data thumbnail store / clear (a file below the title's save root) |
| aoc:u (2) | 200, 302 | required add-on content ids kept; UpdateCachedList re-reads `manifest.json` |
| IApplicationDisplayService (2) | 103, 2460 | indirect binder driver (same IHOSBinderDriver as the relay), image memory info (RGBA rounded up to 0x20000, 0x1000 aligned) |
| caps:u (4) + session (5) | 144-147; 2001-2005 | the 5-variant album lists list the same album; movie read streams read entries stored as movies (a screenshot is "invalid", an absent entry "not found", a deleted movie ends its stream) |
| time (4) | ISystemClock 4; ITimeZoneService 1, 6; time:u 201 | clock operation event signalled by SetCurrentTime / SetSystemClockContext; SetDeviceLocationName (UTC or the host zone, else the usual 116-200) with the updated time point; auto-correction updated time point |
| pdm:qry (5) | 4, 5, 11, 12, 17 | the 10.0.0+ ids of the statistics / account event queries (same handlers as 1, 2, 9, 10); QueryLastPlayTime per requested title (the running title: valid, launched in the past, 0 minutes ago) |
| ILogger (2) | 2, 3 | hashed log / dev notify packets go to the runtime log |
| ngc:u (2) | 4, 5 | Check2 / Mask2 answer like Check / Mask; the word lists now come from the firmware NgWord archive (`sysdata/0100000000000806.bin`, 3043 words from the real 22.1.0 firmware; `NAPPLE_NGC_WORDLIST` still wins) |
| IAudioOut (2) | 12, 13 | the new per-stream gain |
| ISslConnection (4), nsd:u (2), prepo:u (3) | 17, 18, 34, 35; 15, 63; 20102, 20200, 30100 | session cache mode and I/O timeout per connection; change-environment-identifier flag; the 21.0.0 / earlier numberings of SaveSystemReport and ClearStorage, report operation mode |

Also: `GetBleScanFilterParameter` answered 0x10 zero bytes; the documented `BleAdvertisePacketParameter` is 8 bytes (corrected, still a documented default).

## Rows that stay documented defaults (`IPC_STUB`)
- **Network**: IManagerForApplication 2, 3, 4, 150, 160, 170; acc:u0 102, 103, 120, 130, 131; aoc:u 100, 101, 110 (e-shop managers); fsp-srv 81-88 (save data transfer / mover / repair); every IGeneralService / IRequest / IFriendService / ISslContext / ISslConnection / bsd / sfdnsres / bcat / olsc / npns / notif row.
- **No local data to answer from, or arguments undocumented**: fsp-srv 0, 31 (game card), 37, 80, 84, 110, 606, 610, 618, 800, 1001, 1012; IFileSystem 15, 16; aoc:u 300, 301; IApplicationDisplayService 2102, 2450, 2451; ILogger 11; IBtmUserCore 1, 2 (the system's scan parameter values are unknown); IFileSystem QueryEntry / GetFileSystemAttribute.

## Honest notes on the evidence
- Argument layouts taken from the switchbrew tables: fsp-srv 7, 10, 67, 68, 201, 205, 616, 130, 1004, 1011, pdm 4/5/11/12/17, ngc 4/5 (same as 1/2; undocumented), acc 52, vi 2460 (formula from a public emulator's behaviour: rows are `size = ceil(w*h*4 / 0x20000) * 0x20000`, alignment 0x1000).
- Layouts inferred from the command names / typical structure, so unverified against a title: fsp-srv 64, 100, 612, 620, time 6 (name at 0, time point at 0x28), acc 110 / 111 (uid + thumbnail buffer), aoc 200, caps movie streams 2001-2005, ILogger 2/3, SSL 34/35, nsd 15/63, prepo 20200, `IsExFatSupported` always true, `GetProgramIndexForAccessLog` = {1, 0}.
- No `FindSaveDataWithFilter` / OpenFileSystemWithId / profile digest etc. call by a real title was seen in the four-title boots (see the integrator file for what was hit).
