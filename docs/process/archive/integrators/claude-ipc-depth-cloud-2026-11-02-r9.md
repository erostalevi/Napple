---
branch: claude/ipc-depth-cloud-2026-11-02-r9
orchestration: ipc-depth-cloud
env: linux-cloud
status: integrated
---

# Integrator notes: claude/ipc-depth-cloud-2026-11-02-r9

Cloud worker on Linux. Base: `main` = `57431a7`. The change note is `docs/archive/changes/2026-11-02-claude-ipc-depth-cloud-2026-11-02-r9.md`. Every code commit
starts with `unbuilt:`: none of this was compiled on macOS.

## Summary
- **98 IPC rows made real:**
  - 11 documented-default `IPC_STUB` rows converted to `IPC_IMPL`;
  - 87 new `IPC_IMPL` rows for ids that the switchbrew reference fixtures list and that had no row.
- **Counts across `runtime/src`:** 1473 impl / 257 stub before, 1571 impl / 246 stub after.
- **Reference check:** `test_ipc_reference` (built and run on Linux with gcc) still reports `{"fixtures":111,"app_reachable":1325,"missing":0,"errors":0}`.
- **Why 87 of the 98 are new rows:** only about 50 stub rows were in files this session could edit and were not network rows. Of those, the ones with
  undocumented arguments were left as documented defaults. The rest of the depth comes from new, state-backed rows in set:sys, time, vi display, pctl,
  pdm, lbl and ncm. Most of these are `n` (not application-reachable) in the fixtures, so they raise depth without changing what an application can
  reach.

## Files
New:
- `runtime/src/services/sysset_core.[ch]`: the set:sys settings store.
- `runtime/src/services/notif_core.[ch]`: the notif:a alarm store.
- `runtime/src/services/pctl_core.[ch]`: parental-control PIN, rating organization and free-communication list.

Changed services:
- `settings.c`, `time.c`, `time_core.[ch]`, `vi.c`, `misc.c` (aoc and pctl), `push.c` (notif:a), `bcat.c`, `pdm.c`, `lbl.c`, `ncm.c`.

Tests:
- `runtime/tests/test_svc_{settings,time,vi,aoc,pctl,push,bcat,pdm,lbl,ncm}.c`.

Docs:
- the change note above;
- a section appended to `docs/architecture/runtime-services.md`;
- this file.
- Not done: the change note still needs its line in `docs/archive/changes/README.md`. That index is outside this session's ownership, so the
  integrator should add it.

No file owned by another session was touched: fs.c, account*, online_core.c, the nifm / bsd / ssl / nsd / friend part of offline.c, ldn*, applet*, net/
and cloud/.

## Rows converted (stub to impl, 11)
| Interface | Id | Name | Now |
|---|--:|---|---|
| IApplicationDisplayService | 2450 | GetIndirectLayerImageMap | RGBA8 transparent frame of the requested size, `{size, stride}`, size checks as 2460 |
| IApplicationDisplayService | 2451 | GetIndirectLayerImageCropMap | the same, plus a normalised crop rectangle check |
| aoc:u | 100 | CreateEcPurchasedEventManager | an IPurchaseEventManager object |
| aoc:u | 101 | CreatePermanentEcPurchasedEventManager | the same, flagged permanent |
| notif:a | 500 | RegisterAlarmSetting | stores the alarm and returns a u16 id |
| notif:a | 510 | UpdateAlarmSetting | replaces the alarm and its parameter, found by id |
| notif:a | 520 | ListAlarmSettings | the stored settings, in registration order |
| notif:a | 530 | LoadApplicationParameter | the stored parameter and its size |
| notif:a | 540 | DeleteAlarmSetting | removes the alarm; unknown id gives not-found |
| notif:a | 1000 | Initialize | marks the store initialized |
| IBcatService | 20101 | RequestSyncDeliveryCacheWithApplicationIdAndDirectoryName | the same progress object as 10101 (follows the provider) |

## New rows (87)
- **set:sys (36):** 0 SetLanguageCode; 1 / 2 network settings; 7 / 8 lock screen; 9 / 10 backlight; 11 / 12 bluetooth devices; 13 / 14 external steady
  clock source; 15 / 16 user system clock context; 17 / 18 account settings; 19 / 20 audio volume; 21 / 22 EULA versions; 24 SetColorSetId; 25 / 26
  console information upload flag; 27 / 28 automatic application download flag; 29 / 30 notification settings; 47 / 48 quest flag; 54
  SetDeviceTimeZoneLocationName; 57 SetRegionCode; 68 GetSerialNumber; 78 SetDeviceNickName; 150 / 151 time zone location updated time; 183 / 184
  platform region.
- **set (1):** 17 GetKeyboardLayout (EnglishUs, matching GetKeyCodeMap).
- **time (5):** time:u 50 SetStandardSteadyClockInternalOffset and 51 GetStandardSteadyClockRtcValue; ITimeZoneService 7
  SetDeviceLocationNameWithTimeZoneRule, 8 ParseTimeZoneBinary and 20 GetDeviceLocationNameOperationEventReadableHandle.
- **IPurchaseEventManager (5):** 0 SetDefaultDeliveryTarget, 1 SetDeliveryTarget, 2 GetPurchasedEventReadableHandle, 3 PopPurchasedProductInfo,
  4 PopPurchasedProductInfoWithUid.
- **pdm:qry (2):** 18 / 19 QueryApplicationPlayStatistics[ByUserAccountId]ForSystem.
- **lbl (2):** 20 GetCurrentBrightnessMapping, 22 GetCurrentAmbientLightSensorMapping. Rows 19 / 21 now store their mapping.
- **IContentMetaDatabase (3):** 19 GetRequiredApplicationVersion, 22 GetOwnerApplicationId, 23 GetContentAccessibilities.
- **IParentalControlService (16):**
  - 1037 / 1038 rating organization;
  - 1039 / 1042 / 1044 / 1045 / 1049 / 1050 free-communication list;
  - 1201 / 1202 / 1209 unlocks;
  - 1203 SetPinCode, 1206 GetPinCodeLength, 1207 GetPinCodeChangedEvent, 1208 GetPinCode;
  - 1406 GetSettingsLastUpdated;
  - rows 1006 / 1007 are now stateful.
- **ISystemDisplayService (14):** 1203 GetDisplayLogicalResolution, 1204 SetDisplayMagnification, 3204 / 3205 content type, 3206 / 3207 RGB range,
  3208 / 3209 CMU mode, 3210 / 3211 contrast ratio, 3214 / 3215 gamma, 3216 / 3217 CMU luma.
- **IManagerDisplayService (3):** 4201 SetDisplayAlpha, 4205 SetDisplayPowerState, 4209 IsDisplayPanelOn.

## Tested on Linux, and how
**Harness.** `tools/linux-check` with its `ipc_mock.c`: the test #includes the service .c and links every `*_core.c`. Each touched test ran with gcc,
with clang (settings, time, vi, pctl, push) and with `gcc -fsanitize=address,undefined` (`ASAN_OPTIONS=detect_leaks=0`). clang's ASan runtime is
missing in this container.

**Results:**
| Test | Checks | Failed |
|---|--:|--:|
| test_svc_settings | 194 | 0 |
| test_svc_time | 153 | 0 |
| test_svc_vi | 201 | 0 |
| test_svc_aoc | 87 | 0 |
| test_svc_pctl | 215 | 0 |
| test_svc_push | 58 | 0 |
| test_svc_bcat | 148 | 0 |
| test_svc_pdm | 58 | 0 |
| test_svc_lbl | 90 | 0 |
| test_svc_ncm | 146 | 0 |

**Syntax check.** `tools/linux-check/check-syntax.sh` is clean (gcc and clang) on every touched service file.

**Pre-existing Linux link gaps.** test_svc_time and test_svc_vi do not link with the stock `ipc_mock.c`, on `main` too: `host_symbols_register`, and
the timescale and mods symbols are missing. I linked them against a scratch-only stub file outside the repo (weak no-op definitions). `tools/linux-check`
is outside this session's ownership, so the mock was not changed. On a Mac the real objects link.

**Full harness.** `tools/linux-check/run-tests.sh` ran on this branch and on `main` (a worktree at `57431a7`). Both give the same result: 50 test binaries
ran. The same 20 tests fail to build on Linux on both: applet*, audio, fs*, guest_exit, hid, movie_host, ngc, sysconfig, time and vi; they need macOS-only
objects or symbols the mock lacks. The same 2 tests fail on both: test_svc_mii line 35 (host user name) and test_svc_nifm_audit line 54 (system tick).
No new failure.

## Unbuilt (never compiled on macOS)
Every changed or new file under `runtime/src/services/` and `runtime/tests/` listed above.

## How to build and test on the Mac
```
make -C runtime GFX=napple test test-services test-ipc-reference
```
- New `*_core.c` files are picked up by the `src/services/*.c` wildcard.
- The tests that include `misc.c` (test_svc_pctl, test_svc_aoc) now also need `settings_core.o` and `pctl_core.o`. Both come from `TESTOBJ`.
- test_svc_settings needs `sysset_core.o`.

## Risks
- **Layouts are from switchbrew / libnx and were never checked on a console.** The least certain:
  - the notif:a AlarmSetting size (0x40) and parameter limit (0x400);
  - the pctl PIN buffer, read from the first input buffer or else the raw arguments;
  - the argument order of the ISystemDisplayService 32xx rows (value, pad, u64 display);
  - PurchasedProductInfo.
- **Result values are napple's own.** These were not documented and are marked `[unverified]` in the source:
  - notif module 252;
  - pctl wrong PIN `HZ_RESULT(142, 104)`;
  - NIM-shop "nothing to pop" `HZ_RESULT(166, 400)`;
  - ncm invalid key `HZ_RESULT(5, 240)`;
  - set invalid audio target.
- **Defaults are napple's choice:** audio volume 8, backlight 0.5, gamma 2.2, contrast 0.5, rating organization by `NAPPLE_REGION` (PEGI by default).
- **Behaviour change for titles that already ran:**
  - vi 2450 / 2451 now succeed with a transparent image, where they failed before (inline swkbd users read it every frame);
  - aoc 100 / 101 now succeed; PopPurchasedProductInfo then fails with the "nothing to pop" result.
- **Finding, not changed here:** `ncm_core.c` reads the ContentInfo content type at byte 0x17. libnx `NcmContentInfo` has `content_type` at 0x16 and
  `id_offset` at 0x17, so GetContentIdByType may pick the wrong content for real blobs. Worth a separate check.
- **Test fixes:**
  - `test_svc_vi.c` line 104 had a one-word reply buffer that the binder reply (two words) overflowed. ASan found it; the fix is test-only.
  - `test_svc_pctl.c` expected aoc 100 / 101 to be stubs; it is updated.

## Commit note
`b992833` repeats the subject of `c1c93cf` (vi 2450 / 2451). Its only change is the one-line `test_svc_vi.c` buffer fix that ASan found. It was left
as is rather than rewriting the already pushed branch; squash it into `c1c93cf` when integrating if you like.

## Integration result
- Date 2026-11-02, merged into main as fc04ba1 (head 3b5e0b7, rebased by the orchestrator onto local main 21e785e). No conflicts. Outcome: **Clean**
  (no macOS fix was needed), plus one doc commit (the change note's row in the r9 change archive, which the file asked for).
- Every `unbuilt:` commit compiled on macOS for the first time here: `make -C runtime GFX=napple` rc 0, no new warnings (only the three pre-existing ones).
- `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0 on the branch tree and again on main after the merge (with online-race's
  offline.c change): ipc_reference 111 fixtures / app_reachable 1325 / missing 0 / errors 0. Touched suites on macOS: settings 194, time 153, vi 201,
  aoc 87, pctl 215, push 58, bcat 148, pdm 51 (Linux run: 58; all pass, the macOS build of the test checks fewer), lbl 90, ncm 146: 0 failed each.
  test_svc_time / test_svc_vi link with the real objects (the Linux link gap does not exist here).
- Four-title 45 s boots, offline and `NAPPLE_NET=host`: SMO, LAS, MK8D, ACNH rc 0, notify-running, `missing_ipc []`, `unsupported_svcs []`, stubbed only `nvdrv:13`.
- No native ports: pure service logic and state stores; left as C on purpose. The duplicate vi commit subject (4e9e48b / 840ae0a) was left as is.
- Open (from Risks): layouts never checked on a console (notif AlarmSetting size, pctl PIN buffer, ISystemDisplayService 32xx argument order,
  PurchasedProductInfo); napple-chosen result values; `ncm_core.c` reads the ContentInfo type at 0x17 (libnx: 0x16) — worth a separate check.
