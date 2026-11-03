# 2026-11-02 ipc-reference-audit (batch 2026-11-02-r4, mac)

`runtime/build/tests/test_ipc_reference runtime -v` prints a note for every row whose name differs from the reference fixture at the same id (`is named X, the reference says Y`). There were 67. This round resolves 65 of them; the two left are in `applet.c` (not in this item's area). Status words: **tested** = unit test passes here, **verified** = a real title called it.

## Evidence
The fixtures in `runtime/tests/ipc_reference/` were authored from the switchbrew command tables. I re-read the relevant switchbrew pages (Shared Database services, Parental Control services, NS services, Filesystem services, Sockets services, SSL services, LDN services, Network Interface services, Glue services, OLSC services, Capture services, BCAT services, Audio services, HID services) and compared every mismatched id to the page text. In every case the fixture agreed with the page; the **source rows were wrong**, so no fixture was changed. Only ids and names come from the pages; argument layouts of rows made real here are marked below.

## Mismatches resolved (65)
Rows where the id was wrong (a real title calling the documented id would have reached another handler or an "unknown command"):

| Interface | Was | Now (switchbrew) | Test |
|---|---|---|---|
| `pctl` | 1018 IsRestrictionTemporaryUnlocked, 1035 IsFreeCommunicationDefaultValue, 1061 ConfirmStereoVisionPermission, 1062 EndFreeCommunication, 1451 IsStereoVisionPermitted | 1018 IsFreeCommunicationAvailable, 1035 GetCurrentSettings (12 zero bytes), 1061 ConfirmStereoVisionRestrictionConfigurable, 1062 GetStereoVisionRestriction (false), 1065 IsStereoVisionPermitted (new), 1451 StartPlayTimer | `test_svc_pctl.c` |
| `pdm:qry` | 1, 2, 6, 7, 8, 9, 10 (invented numbering) | 4 / 5 statistics, 8 QueryPlayEvent, 9 GetAvailablePlayEventRange, 10 QueryAccountEvent (11, 12, 17 were already right) | `test_svc_pdm.c` |
| `ISteadyClock` | 200 GetRtcValue, 201 IsRtcResetDetected, 204 GetInternalOffset | 100, 101 (a bool), 102 GetSetupResultValue (new), 200 GetInternalOffset; Get/SetTestOffset now keep a TimeSpanType that shifts the time point | `test_svc_time.c` |
| `bsd:u` | 28 stub, 29 GetResourceStatistics, 30 RecvMMsg, 31 SendMMsg | 28 GetResourceStatistics, 29 RecvMMsg, 30 SendMMsg, 31 EventFd ({-1, EOPNOTSUPP}, documented default) | `test_svc_offline_depth.c`, `test_svc_online.c`, `test_svc_nifm_audit.c` |
| `ISslConnection` | 19 SetSessionCacheMode, 20 GetSessionCacheMode, 21 FlushSessionCache (duplicates of 17 / 18) | 19 FlushSessionCache, 20 SetRenegotiationMode, 21 GetRenegotiationMode (new, kept per connection) | `test_svc_local_offline.c`, `test_svc_offline_depth.c` |
| `IGeneralService` | 15 GetInternetConnectionStatus, 16 IsWirelessCommunicationEnabled, 17 GetInternetConnectionStatusEx, 18 / 19 / 20 stubs | 15 GetCurrentIpConfigInfo (no connection), 16 SetWirelessCommunicationEnabled, 17 IsWirelessCommunicationEnabled, 18 GetInternetConnectionStatus, 19 / 20 Set/IsEthernetCommunicationEnabled | `test_svc_nifm_audit.c`, `test_svc_online.c`, `test_svc_offline.c` |
| `fsp-srv` | 30 OpenDataStorageByDataId (a duplicate of 202) | 30 OpenGameCardStorage (no game card: documented default) | `test_svc_fs_dataid.c` moved to 202 |
| `miiimg` | 0-15 own numbering (DestroFile, GetFreeSlotCount, Reserved14 ...) | 0 Initialize, 10 Reload, 11 GetCount, 12 IsEmpty, 13 IsFull, 14 GetAttribute, 15 LoadImage, 16 AddOrUpdateImage, 17 DeleteImages, 18-20 migration (default), 100 DeleteFile, 101 DestroyFile, 102 ImportFile, 103 ExportFile, 104 ForceInitialize | `test_svc_mii_img.c` |
| `olsc:u` | 1-9, 11, 12 (guessed rows), 10 / 11 opening the controllers, 13 DeleteUserSaveDataInSyncArea, 14-16 sync-area calls | 0 Initialize, 10 VerifySaveDataBackupLicenseAsync (offline), 13 GetSaveDataBackupSetting, 14 SetSaveDataBackupSettingEnabled, 15 SetCustomData, 16 DeleteSaveDataBackupSetting (all succeed, zero output: Animal Crossing 3.0.3 calls 13 at boot and ends the process on an error), 18-27 as before. `ITransferTaskListController` / `IRemoteStorageController` keep the switchbrew ids but have no row to reach them (they belong to `olsc:s`) | `test_svc_olsc.c` |
| `ectx:aw` | 0 SetResult, 1 SetResultWithContext | 0 CreateContextRegistrar (returns an `IContextRegistrar`, 0 Complete), 1 CommitContext | `test_svc_ectx.c` |
| `prepo:u` | 10100 SaveReport ... 10105 SaveReportWithUserForApplication, 90100 ClearStorage, 90200 GetStorageUsage | 10100-10107 SaveReport(WithUser) Old / Old2 / Old3 / current, 20100-20103, 30300 GetStorageUsage, 30100 ClearStorage, 90100 ReadAllReportFiles (default); the invented 90200 is gone | `test_svc_nifm_audit.c`, `test_svc_offline_depth.c`, `test_svc_local_offline.c` |
| `aoc:u` | 13 NotifyMountAddOnContentWithProcessId (a no-op), 14 | 13 IsAddOnContentMountedForDebug (a bool; 11 / 12 now keep a mount count) | `test_svc_pctl.c` |

Name-only (same id, same behaviour): `IAudioIn` 1, 2, 5, 12, 13; `IAudioOut` 1, 2, 5; `IFinalOutputRecorder` 1, 2; `IBcatService` 30200, 30201, 90100; `ISslContext` 6 (+ 7 RemoveClientPki now real); `IUserLocalCommunicationService` 100 GetStateChangeEvent, 101 GetNetworkInfoAndHistory, 104 SetWirelessControllerPolicy, 402 InitializeWithVersion; `caps:u` 142, 143 (`...Deprecated`); `irs` 312 RunPointingProcessor; `sfdnsres` 8 GetCancelHandleRequest.

### Not resolved here (both in `applet.c`, am-stubs-legal's file)
- `IApplicationFunctions` 210 is named `GetSystemEvent210`; the reference says (unnamed) (switchbrew: `Cmd210`, 20.0.0+). Name-only.
- The am `IStorage` object (`applet.c`, `0 Open`) shares the interface name `IStorage` with fsp-srv's `IStorage`, so the checker compares it with `0 Read`. Renaming the am object (for example `IStorageAm`) removes the note; no behaviour changes.

## nifm answers like an offline console
- `IRequest::GetResult` (1): the request's result (0 before Submit; `2110-1111`-class "communication disabled" after the NullProvider rejects; cleared by Cancel). Already real; now pinned by a test.
- `GetAppletInfo` (21): switchbrew gives `in u32 theme color, out buffer (0x1000), out u32 AppletId, u32 LibraryAppletMode, u32 out_size`, buffer = array of `{s32 size, data}` ended by `size == -1`, AppletId one of error / wifiWebAuth / netConnect. A request that failed is answered with **netConnect (0x0f, the id `applet_lib_core.c` uses for the network-connection applet), mode 0 (all foreground), no extra input storage** (buffer = the `-1` terminator, `out_size` 4). A request that did not fail (never submitted, cancelled) answers "communication disabled": there is nothing for an applet to fix. **Inferred**: that a failed request with the NullProvider is the netConnect case (rather than `error`), and that the argument array is empty.
- `GetRequirement` (19): 0x24 bytes (0x20 plus 4 since 3.0.0): what `SetRequirement` stored, zero-padded. `SetRequirementByRevision` (18) / `GetRevision` (20): a stored word.
- `GetAdditionalInfo` (22): no layout is documented; it reports the request's failure and stays a documented default.
- `SetGreedy`, `SetSharable`, `SetKeptInSleep`, `Register/UnregisterSocketDescriptor` (16, 17, 23, 24, 25): kept per request.
- `IGeneralService`: wireless on and ethernet off by default, the acceptable-network-type flags, the default IP setting buffer, background-request flag and the two Wake-on-WLAN values are stored service-wide and read back (16, 17, 19, 20, 28, 27, 34, 40, 41, 43, 57).

## Rows made real (documented defaults -> `IPC_IMPL`, 17 rows, plus the new ones above)
`IRequest` 16, 17, 18, 19, 20, 21, 23, 24, 25 (9); `IGeneralService` 19, 20, 28, 34, 40, 41, 43, 57 (8). New real rows that did not exist: `ISteadyClock` 102, `ISslConnection` 20 / 21, `pctl` 1065, `IContextRegistrar::Complete`. Rows of the assignment's list that stay documented defaults, with the reason: fsp-srv 0, 31, 37, 80, 84, 110, 606, 610, 618, 800, 1001, 1012 and IFileSystem 15 / 16 (no game card / NCA / rights data on the host; `OpenSaveDataMetaFile`, `ListAccessibleSaveDataOwnerId`, `QueryEntry`, `GetFileSystemAttribute` have undocumented layouts); vi 2102 / 2450 / 2451 (undocumented layouts); btm 1 / 2 (the system's scan parameters are unknown); lm 11 (shared memory log accessor, no consumer); ovln `GetUnreceivedMessageCount` (no receiver exists, 0 is the right answer); acc / aoc network rows. Rows that are only a stored setting with no reader (ldn 105, 106, 403, 505, 600, 601) were not counted.

## Honest notes on the evidence
- Ids and names of every changed row: switchbrew tables (read 2026-11-02).
- Return layouts taken from the old handlers and kept: pctl 1018 (no output) and 1062 (one bool), the u8 / u32 words of the nifm settings, GetCurrentSettings = the 12-byte block of GetSafetyLevelSettings (1034).
- Inferred, unverified against a title: nifm GetAppletInfo's applet and empty argument array, GetRequirement padding, wireless default on (it was effectively off before because the old ids hit another handler), ethernet default off, ectx registrar argument layout (only the first word is logged), the miiimg migration rows, prepo 10106 / 10107 sharing the 10104 / 10105 handler.
- No real title was seen calling any renumbered row except as listed in the hand-off notes's boot table.
