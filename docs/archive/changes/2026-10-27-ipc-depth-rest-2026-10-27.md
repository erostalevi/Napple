# 2026-10-27 ipc-depth-rest: stub IPC rows in ncm, ns, ngc, push, btm, hidbus, ldn, offline, sfdnsres become real behaviour, with host tests

Branch claude/ipc-depth-rest-2026-10-27 (Linux cloud session; assignment orchestrations/ipc-depth-rest.md). Base 273606a (orchestrate/2026-10-27-polish).

## Result
Scan-style count (`IPC_IMPL }` / `IPC_STUB }` rows over `runtime/src/ipc.c` + `runtime/src/services/*.c`, the way `percent-analysis/scan.py` counts them): **867 / 267 -> 1004 / 130**
(stub share 23.5% -> 11.5%; the pool of this assignment was 143 stub rows in 9 files: 137 resolved, 6 kept with a reason). The assignment's "about 100" target is exceeded.
Per file (stub rows before -> after): offline 89 -> 3, btm 14 -> 0, hidbus 12 -> 0, ldn 10 -> 0, ns 7 -> 3, ncm 4 -> 0, sfdnsres 4 -> 0, ngc 2 -> 0, push 1 -> 0, caps 0 (nothing to do:
`caps.c` had no stub rows at this base).

## Class legend
(a) implemented with real behaviour (state or parsing) and a test; (b) needs a hardware simulation (none left in this pool: btm/hidbus/ldn keep their "no radio / no device" answers
as complete class (c) answers, a simulator would be a separate package); (c) trivially correct documented no-op or empty/offline answer, switched to impl with a comment;
(d) not needed by any application or needs data we do not have (kept as stub, reason given). Online services stay provider hooks: no Nintendo online code, no endpoints.

## What was built (all tested on Linux; nothing verified on a Mac)
- **ngc**: `ngc_core.[ch]` (new): word matcher over a user-supplied list (`NAPPLE_NGC_WORDLIST`, one word per line); `Check` flags, `Mask` replaces matched UTF-16 units with `*`,
  content version bumps on (re)load. No list = nothing flagged (the old behaviour). No word list is shipped.
- **ncm**: `ncm_core.[ch]` (new): parses the stored ContentMeta blob (8-byte header, extended header, 0x18-byte ContentInfo, 0x10-byte ContentMetaInfo). `GetContentIdByType`,
  `ListContentInfo`, `HasContent`, `ListContentMetaInfo` and `GetAttributes` read from it.
- **ldn**: access point configuration in `ldn_core.[ch]`: `SetAdvertiseData` (<= 0x180 bytes), `SetStationAcceptPolicy` (0..3), `AddAcceptFilterEntry`/`ClearAcceptFilter` (8 entries,
  unique), all only legal once a network is created; `Reject` (no stations: node not found); `SetWirelessControllerRestriction` stored; network queries answer "not available".
- **sfdnsres**: private DNS address list stored/read back; cancel handles recorded.
- **ns**: `ListApplicationRecord` / `GenerateApplicationRecordCount` serve the running title from `g_cfg.title_id` (`ns_core` additions); occupied size and the update event are complete answers.
- **btm**: per-core state for the three BLE scans, a connection event signalled by `BleDisconnect`; still no radio so no results. **hidbus**: handle decode + "not connected" per command,
  status manager type stored. **push**: rows complete for an offline console.
- **offline.c** (89 -> 3): `offline_state_core.[ch]` (new) holds the mm:u clock request table (request ids, Set/Get, legacy by-module calls) and the prepo report store (bytes/count,
  a 4 MiB limit, user-agreement flag). nifm requests keep their parameters, general services get stable client ids and an exclusive client; friend services keep the online-play
  flag; ssl contexts keep options, pki ids and a refcounted connection count; nfc keeps the pass-through session flag; bsd counts clients and answers `GetResourceStatistics`.
- Linux harness: `tests/linux/check_known_calls.py` + `known_calls_ipc_depth.txt` (380 command rows that existed at the base) run from `run.sh`: no row may disappear or be renamed,
  so the calls titles already make keep their ids and names.

## Decisions
- Behaviour a title may already depend on was kept: every old answer is unchanged unless the row now has real state (the old tests pass untouched; `test_svc_ngc` and `test_svc_btm` were extended, not weakened).
- Layouts of newly implemented rows are from memory of the public documentation (unverified) and marked in the comments; unknown or short requests are answered, never fatal.
- `DuplicateSocket`, `RecvMMsg`, `SendMMsg` stay stubs (d): they need descriptor aliasing in `bsd_core` / mmsghdr marshalling and no SDK path reaches them.
  `ns` 400 / 21 / 23 stay stubs (d): control.nacp and the content layout are not written next to the data directory by napple build.
- Not touched (other areas): vi, applet*, mii*, amiibo*, fs, nvdrv, time*, hid, audio, caps.

## Verified vs not
tested (Linux): `runtime/tests/linux/run.sh` -- see the integrator file for the final counts. **Not run (needs a Mac)**: `make -C runtime test test-services test-host`, the four-title 45 s
boots (LAS, SMO, ACNH, MK8D) and a command-trace diff before/after. `offline.c` compiles and its tests run on Linux (it builds there), but the arc4random / Mach-adjacent paths of the
rest of the runtime were not exercised. Pre-existing Linux failure not caused by this work: `svc_mii` ("Get: elements carry the source word and the host user's name", fails at the base too).

## Inventory (stub rows at the base; rows are listed by file + name, so the same name in two objects shows once)
| File | Cmd | Name | Before | Now | Class / reason |
|---|---|---|---|---|---|
| ncm.c | 3 | GetContentIdByType | stub | impl | (a) parsed from the stored CNMT blob (ncm_core.c) |
| ncm.c | 4 | ListContentInfo | stub | impl | (a) parsed from the stored CNMT blob (ncm_core.c) |
| ncm.c | 16 | HasContent | stub | impl | (a) parsed from the stored CNMT blob (ncm_core.c) |
| ncm.c | 17 | ListContentMetaInfo | stub | impl | (a) parsed from the stored CNMT blob (ncm_core.c) |
| ns.c | 0 | ListApplicationRecord | stub | impl | (a) record list of the running title; occupied size 0 is true (not installed on a console storage) [c] |
| ns.c | 1 | GenerateApplicationRecordCount | stub | impl | (a) record list of the running title; occupied size 0 is true (not installed on a console storage) [c] |
| ns.c | 2 | GetApplicationRecordUpdateSystemEvent | stub | impl | (a) record list of the running title; occupied size 0 is true (not installed on a console storage) [c] |
| ns.c | 400 | GetApplicationControlData | stub | stub | (d) kept |
| ns.c | 11 | CalculateApplicationOccupiedSize | stub | impl | (a) record list of the running title; occupied size 0 is true (not installed on a console storage) [c] |
| ns.c | 21 | GetApplicationContentPath | stub | stub | (d) kept |
| ns.c | 23 | ResolveApplicationContentPath | stub | stub | (d) kept |
| ngc.c | 1 | Check | stub | impl | (a) word matcher over a user-supplied list (ngc_core.c); empty list = clean |
| ngc.c | 2 | Mask | stub | impl | (a) word matcher over a user-supplied list (ngc_core.c); empty list = clean |
| btm.c | 0 | AcquireBleScanEvent | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 3 | StartBleScanForGeneral | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 4 | StopBleScanForGeneral | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 5 | GetBleScanResultsForGeneral | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 6 | StartBleScanForPaired | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 7 | StopBleScanForPaired | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 8 | StartBleScanForSmartDevice | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 9 | StopBleScanForSmartDevice | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 10 | GetBleScanResultsForSmartDevice | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 17 | AcquireBleConnectionEvent | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 18 | BleConnect | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 19 | BleOverrideConnection | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 20 | BleDisconnect | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| btm.c | 21 | BleGetConnectionState | stub | impl | (c) no radio: scan/connection state kept, no result ever appears |
| hidbus.c | 3 | Initialize | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 4 | Finalize | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 5 | EnableExternalDevice | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 6 | GetExternalDeviceId | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 7 | SendCommandAsync | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 8 | GetSendCommandAsynceResult | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 9 | SetEventForSendCommandAsycResult | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 10 | GetSharedMemoryHandle | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 11 | EnableJoyPollingReceiveMode | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 12 | DisableJoyPollingReceiveMode | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 13 | GetPollingData | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| hidbus.c | 14 | SetStatusManagerType | stub | impl | (c) no device on the rail: handle-checked "not connected" answers; status manager type stored |
| ldn.c | 3 | GetDisconnectReason | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 4 | GetSecurityParameter | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 5 | GetNetworkConfig | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 101 | GetNetworkInfoLatestUpdate | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 104 | SetWirelessControllerRestriction | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 205 | Reject | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 206 | SetAdvertiseData | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 207 | SetStationAcceptPolicy | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 208 | AddAcceptFilterEntry | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| ldn.c | 209 | ClearAcceptFilter | stub | impl | (a) access point config state (advertise data, accept policy, filter) with state checks; network queries "not available" [c] |
| sfdnsres.c | 0 | SetDnsAddressesPrivateRequest | stub | impl | (a) private DNS list stored; cancel handles recorded |
| sfdnsres.c | 1 | GetDnsAddressPrivateRequest | stub | impl | (a) private DNS list stored; cancel handles recorded |
| sfdnsres.c | 8 | RequestCancelHandleRequest | stub | impl | (a) private DNS list stored; cancel handles recorded |
| sfdnsres.c | 9 | CancelRequest | stub | impl | (a) private DNS list stored; cancel handles recorded |
| offline.c | 0 | InitializeOld | stub | impl | (a) mm clock request table |
| offline.c | 1 | FinalizeOld | stub | impl | (a) mm clock request table |
| offline.c | 2 | SetAndWaitOld | stub | impl | (a) mm clock request table |
| offline.c | 3 | GetOld | stub | impl | (a) mm clock request table |
| offline.c | 4 | Initialize | stub | impl | (a) mm clock request table |
| offline.c | 5 | Finalize | stub | impl | (a) mm clock request table |
| offline.c | 6 | SetAndWait | stub | impl | (a) mm clock request table |
| offline.c | 7 | Get | stub | impl | (a) mm clock request table |
| offline.c | 0 | GetEvent | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 1 | Clear | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 2 | Pop | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 0 | GetCompletionEvent | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 1 | Cancel | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 10400 | GetBlockedUserListIds | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 20101 | GetNewlyFriendCount | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 20200 | GetReceivedFriendRequestCount | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 10600 | DeclareOpenOnlinePlaySession | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 10601 | DeclareCloseOnlinePlaySession | stub | impl | (c) empty queue / no provider hook; play-session flag kept |
| offline.c | 0 | CreateFriendService | stub | impl | (c) object factory |
| offline.c | 1 | CreateNotificationService | stub | impl | (c) object factory |
| offline.c | 2 | CreateDaemonSuspendSessionService | stub | impl | (c) object factory |
| offline.c | 5 | SetRequirement | stub | impl | (a) request/service state |
| offline.c | 6 | SetRequirementPreset | stub | impl | (a) request/service state |
| offline.c | 8 | SetPriority | stub | impl | (a) request/service state |
| offline.c | 9 | SetNetworkProfileId | stub | impl | (a) request/service state |
| offline.c | 10 | SetRejectable | stub | impl | (a) request/service state |
| offline.c | 11 | SetConnectionConfirmationOption | stub | impl | (a) request/service state |
| offline.c | 12 | SetPersistent | stub | impl | (a) request/service state |
| offline.c | 13 | SetInstant | stub | impl | (a) request/service state |
| offline.c | 14 | SetSustainable | stub | impl | (a) request/service state |
| offline.c | 15 | SetRawPriority | stub | impl | (a) request/service state |
| offline.c | 1 | GetClientId | stub | impl | (a) request/service state |
| offline.c | 26 | SetExclusiveClient | stub | impl | (a) request/service state |
| offline.c | 27 | GetDefaultIpSetting | stub | impl | (c) no profile: zeroed IpSettingData |
| offline.c | 4 | CreateGeneralServiceOld | stub | impl | (c) object factory |
| offline.c | 5 | CreateGeneralService | stub | impl | (c) object factory |
| offline.c | 0 | RegisterClient | stub | impl | (c)/(a) client counter, ENOENT for device files, zero statistics |
| offline.c | 1 | StartMonitoring | stub | impl | (c)/(a) client counter, ENOENT for device files, zero statistics |
| offline.c | 4 | Open | stub | impl | (c)/(a) client counter, ENOENT for device files, zero statistics |
| offline.c | 7 | Sysctl | stub | impl | (c)/(a) client counter, ENOENT for device files, zero statistics |
| offline.c | 6 | Write | stub | impl | (c) NTAG tag is not a Mifare card: wrong device state is the answer |
| offline.c | 5 | Read | stub | impl | (c) NTAG tag is not a Mifare card: wrong device state is the answer |
| offline.c | 27 | DuplicateSocket | stub | stub | (d) needs descriptor aliasing / mmsghdr marshalling; no SDK path reaches it |
| offline.c | 29 | GetResourceStatistics | stub | impl | (c)/(a) client counter, ENOENT for device files, zero statistics |
| offline.c | 30 | RecvMMsg | stub | stub | (d) needs descriptor aliasing / mmsghdr marshalling; no SDK path reaches it |
| offline.c | 31 | SendMMsg | stub | stub | (d) needs descriptor aliasing / mmsghdr marshalling; no SDK path reaches it |
| offline.c | 10 | GetSettingName | stub | impl | (c) environment "lp1" |
| offline.c | 11 | GetEnvironmentIdentifier | stub | impl | (c) environment "lp1" |
| offline.c | 12 | GetDeviceId | stub | impl | (c) fixed zero id |
| offline.c | 13 | DeleteSettings | stub | impl | (a) imported blob state |
| offline.c | 14 | ImportSettings | stub | impl | (a) imported blob state |
| offline.c | 20 | Resolve | stub | impl | (c) goes to the provider; the NullProvider returns the name unchanged |
| offline.c | 21 | ResolveEx | stub | impl | (c) same |
| offline.c | 30 | GetNasServiceSetting | stub | impl | (c) no service configured: empty struct |
| offline.c | 31 | GetNasServiceSettingEx | stub | impl | (c) same |
| offline.c | 40 | GetNasRequestFqdn | stub | impl | (c) same |
| offline.c | 41 | GetNasRequestFqdnEx | stub | impl | (c) same |
| offline.c | 42 | GetNasApiFqdn | stub | impl | (c) same |
| offline.c | 43 | GetNasApiFqdnEx | stub | impl | (c) same |
| offline.c | 50 | GetCurrentSetting | stub | impl | (c) same |
| offline.c | 0 | CreateUserInterface | stub | impl | (a) per-object service state / object factory |
| offline.c | 0 | CreateUserInterface | stub | impl | (a) per-object service state / object factory |
| offline.c | 0 | CreateUserInterface | stub | impl | (a) per-object service state / object factory |
| offline.c | 401 | KeepPassThroughSession | stub | impl | (a) per-object service state / object factory |
| offline.c | 402 | ReleasePassThroughSession | stub | impl | (a) per-object service state / object factory |
| offline.c | 10100 | SaveReport | stub | impl | (a) report store / provider hook |
| offline.c | 10101 | SaveReportWithUser | stub | impl | (a) report store / provider hook |
| offline.c | 10102 | SaveReportOld | stub | impl | (a) report store / provider hook |
| offline.c | 10103 | SaveReportWithUserOld | stub | impl | (a) report store / provider hook |
| offline.c | 10104 | SaveReportForApplication | stub | impl | (a) report store / provider hook |
| offline.c | 10105 | SaveReportWithUserForApplication | stub | impl | (a) report store / provider hook |
| offline.c | 10200 | RequestImmediateTransmission | stub | impl | (c) offline: nothing queued to transmit |
| offline.c | 10300 | GetTransmissionStatus | stub | impl | (c) offline: nothing queued to transmit |
| offline.c | 10400 | GetSystemSessionId | stub | impl | (c) offline: nothing queued to transmit |
| offline.c | 20100 | SaveSystemReport | stub | impl | (a) report store / provider hook |
| offline.c | 20101 | SaveSystemReportWithUser | stub | impl | (a) report store / provider hook |
| offline.c | 40100 | IsUserAgreementCheckEnabled | stub | impl | (a) report store / provider hook |
| offline.c | 40101 | SetUserAgreementCheckEnabled | stub | impl | (a) report store / provider hook |
| offline.c | 90100 | ClearStorage | stub | impl | (a) report store / provider hook |
| offline.c | 90200 | GetStorageUsage | stub | impl | (a) report store / provider hook |
| offline.c | 21 | FlushSessionCache | stub | impl | (a) context options / pki ids / counts |
| offline.c | 0 | SetOption | stub | impl | (a) context options / pki ids / counts |
| offline.c | 1 | GetOption | stub | impl | (a) context options / pki ids / counts |
| offline.c | 3 | GetConnectionCount | stub | impl | (a) context options / pki ids / counts |
| offline.c | 4 | ImportServerPki | stub | impl | (a) context options / pki ids / counts |
| offline.c | 5 | ImportClientPki | stub | impl | (a) context options / pki ids / counts |
| offline.c | 6 | RemovePki | stub | impl | (a) context options / pki ids / counts |
| offline.c | 1 | GetContextCount | stub | impl | (a) context options / pki ids / counts |
| offline.c | 5 | SetInterfaceVersion | stub | impl | (a) context options / pki ids / counts |
