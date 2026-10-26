# 2026-10-26 runtime-depth: stub command rows become real behaviour, with host tests

Branch claude/runtime-depth-2026-10-26-macos1 (Linux cloud session; assignment orchestrations/runtime-depth.md). Base b48737f.

## Result
The scan's "IPC: command depth (impl vs stub)" went from 52% (524 implemented / 494 stub rows) to **68%** (695 / 327) (domain points 2.6 → 3.4 of 5). Stub rows (`IPC_STUB`) in the
files of this assignment and the unowned files I took on as stretch: 230 before, 167 resolved, 63 kept (all with a reason in the inventory). hid, bcat, audio
(audren/audout/audin/hwopus), psm, lbl, settings, apm, pdm and pctl (misc.c) have **0** stub rows left. Rows owned by other sessions (irs, offline, sfdnsres, account,
mii, applet, fs: 241 rows) were not touched.

## Class legend
(a) implemented now with real behaviour and a test; (b) needs a hardware simulation (left for sim-online); (c) trivially correct documented no-op or empty/offline answer, switched
to impl with a comment; (d) not needed by any application or needs data we do not have (kept as stub, reason given).

## What was built (all tested on Linux; none verified on a Mac)
- **Linux test harness** `runtime/tests/linux/run.sh` (+ `mach_shim.h`, `ipc_mock.c`): builds and runs every `tests/test_svc_*.c` against the real kernel/ipc/threads objects, the audio
  library (needs libopus-dev) and the input library. 32 suites run, 5 do not build on Linux (applet, applet_libs, fs, fs_dataid, time: they need host symbols). `input/Makefile.linux`
  (+ `tests/linux/ng_stub.c` in place of GameController) runs the input tests; `audio/Makefile.linux` already existed.
- **hid** (24 → 0): debug pad (real section, player 1's pad mapped to the debug pad bits), gesture (new portable recogniser `input/src/gesture.c`: Touch, Press, Tap/double tap, Pan, Swipe with
  direction and velocity, Pinch, Rotate, Complete; written to a new gesture lifo), console six-axis record, `IsSixAxisSensorAtRest` from the real sensor sample
  (`napple_sixaxis_at_rest`), npad assignment (Deactivate/Activate, Disconnect, SwapNpadAssignment, MergeSingleJoyAsDualJoy, LR assignment mode), capture-button assignment table,
  keyboard lock keys (shown as modifier bits), `SetTouchScreenResolution` (changes the touch/mouse coordinate space), touch configuration, vibration device list
  validation and `IsVibrationDeviceMounted`, xpad = none. New input API: `napple_input_set_assignment/_set_screen_size/_set_debug_pad_enabled/_set_gesture_enabled/_set_console_sixaxis_enabled/_get_sixaxis/_touch_points`
  (multi-touch frames for a future touch frontend).
- **bcat** (21 → 0): empty delivery cache answers, passphrase size validation, background-task rows as documented no-ops.
- **audio** (10 → 0): `GetAudioOutBufferCount/PlayedSampleCount/FlushAudioOutBuffers` backed by new `napple_audout_buffer_count/_played_samples/_flush`; device events fire on
  `audio_device_changed()`; `OpenAudioRendererForManualExecution` now really opens a manual renderer and `ExecuteAudioRendererRendering` renders one frame; opus SetContext documented no-op;
  the audren performance frame now follows a v1 (REV1-4, 0x10 header {entries, details, next offset, total time}) / v2 (REV5+, 0x20 header with `PERF` magic, voices dropped, start time, frame
  index) layout. **The field order and the REV5 split are from memory and unverified** (see Risks). audin: gain state.
- **psm** (9 → 0): session and charge-info events fire when the host power situation changes (`psm_host_power_changed`) or enough-power emulation toggles; per-kind enable masks; controller power
  supply acquire/release counted. **lbl** (9 → 0): Save/Load persistence of the setting, ambient sensor value, reflection delay per direction, VR apply.
  **settings** (4 → 0): region from the host language, colour set, time zone name from env (`settings_core.c`). **apm** (2 → 0): per-mode configuration + clock table (integration 2026-10-26: the docked-only refusal was removed, every id is accepted in both modes; Link's Awakening sets 0x20004 for handheld and stopped with 0x67f on the refusal) (`apm_core.c`, unverified values), overclock flag.
  **pdm** (6 → 0), **pctl/aoc** (17 → 0): documented answers (controls off; DLC set immutable).
- Stretch (files nobody owns): spsm, ovln, ectx, fgm, olsc (23 → 0), time (8 → 0, `GetRtcValue` now returns the host clock in the RTC epoch), ns (22 → 7), ncm (7 → 4), ngc (4 → 2), audin (2 → 0).

## Decisions
- A row is only switched to impl when the answer is complete for this host (class c) or backed by state and a test (class a). Rows that need data we do not have stay stubs and are listed.
- Behaviour that could regress titles was kept lenient: argument layouts of rarely used hid commands are read with bounds checks; `SendVibrationValue` still reaches the pad without prior activation;
  Mount/Unmount of add-on content still always succeed.
- hid.c: the Mach-only shared-memory read trace is now `#ifdef __APPLE__` so the file also compiles on Linux (behaviour on macOS unchanged).
- New env vars (all optional): `NAPPLE_REGION`, `NAPPLE_COLOR_SET`, `NAPPLE_TIMEZONE`.

## Verified vs not
tested (Linux): runtime `tests/linux/run.sh` 32 suites 0 failures; `input/Makefile.linux` 6 suites; `audio/Makefile.linux` 10 suites. **Not run (needs a Mac)**: `make -C runtime test test-services test-host`,
`make -C input test`, `make -C audio test`, the four-title 45 s boots, and a command-trace diff before/after (no game data here). `time.c` was only syntax-checked (its test does not link on Linux).
`cargo` gates were not touched by this branch. clang-format applied to touched sources.

## Inventory (stub rows before this work; 167 resolved, 63 kept; other sessions' files omitted)
| File | Cmd | Name | Now | Class / reason |
|---|---|---|---|---|
| hid.c | 0 | ActivateVibrationDevice | impl | (a) state + input layer |
| hid.c | 32 | SendKeyboardLockKeyEvent | impl | (a) state + input layer |
| hid.c | 40 | AcquireXpadIdEventHandle | impl | (a) state + input layer |
| hid.c | 41 | ReleaseXpadIdEventHandle | impl | (a) state + input layer |
| hid.c | 51 | ActivateXpad | impl | (a) state + input layer |
| hid.c | 55 | GetXpadIds | impl | (a) state + input layer |
| hid.c | 82 | IsSixAxisSensorAtRest | impl | (a) state + input layer |
| hid.c | 91 | ActivateGesture | impl | (a) state + input layer |
| hid.c | 104 | DeactivateNpad | impl | (a) state + input layer |
| hid.c | 107 | DisconnectNpad | impl | (a) state + input layer |
| hid.c | 125 | MergeSingleJoyAsDualJoy | impl | (a) state + input layer |
| hid.c | 126 | StartLrAssignmentMode | impl | (a) state + input layer |
| hid.c | 127 | StopLrAssignmentMode | impl | (a) state + input layer |
| hid.c | 130 | SwapNpadAssignment | impl | (a) state + input layer |
| hid.c | 133 | SetNpadAnalogStickUseCenterClamp | impl | (a) state + input layer |
| hid.c | 134 | SetNpadCaptureButtonAssignment | impl | (a) state + input layer |
| hid.c | 135 | ClearNpadCaptureButtonAssignment | impl | (a) state + input layer |
| hid.c | 211 | IsVibrationDeviceMounted | impl | (a) state + input layer |
| hid.c | 300 | ActivateConsoleSixAxisSensor | impl | (a) state + input layer |
| hid.c | 301 | StartConsoleSixAxisSensor | impl | (a) state + input layer |
| hid.c | 302 | StopConsoleSixAxisSensor | impl | (a) state + input layer |
| hid.c | 1002 | SetTouchScreenConfiguration | impl | (a) state + input layer |
| hid.c | 1003 | IsFirmwareUpdateNeededForNotification | impl | (a) state + input layer |
| hid.c | 1004 | SetTouchScreenResolution | impl | (a) state + input layer |
| bcat.c | 0 | GetEvent | impl | (c) empty delivery cache is the console answer |
| bcat.c | 1 | Read | impl | (c) empty delivery cache is the console answer |
| bcat.c | 2 | GetCount | impl | (c) empty delivery cache is the console answer |
| bcat.c | 3 | Close | impl | (c) empty delivery cache is the console answer |
| bcat.c | 1 | Read | impl | (c) empty delivery cache is the console answer |
| bcat.c | 2 | GetSize | impl | (c) empty delivery cache is the console answer |
| bcat.c | 3 | GetDigest | impl | (c) empty delivery cache is the console answer |
| bcat.c | 10 | EnumerateDeliveryCacheDirectory | impl | (c) empty delivery cache is the console answer |
| bcat.c | 10100 | RequestSyncDeliveryCache | impl | (c) empty delivery cache is the console answer |
| bcat.c | 10101 | RequestSyncDeliveryCacheWithDirectoryName | impl | (c) empty delivery cache is the console answer |
| bcat.c | 10200 | CancelSyncDeliveryCacheRequest | impl | (c) empty delivery cache is the console answer |
| bcat.c | 20100 | RequestSyncDeliveryCacheWithApplicationId | impl | (c) empty delivery cache is the console answer |
| bcat.c | 30100 | SetPassphrase | impl | (c) empty delivery cache is the console answer |
| bcat.c | 30200 | RegisterBackgroundDeliveryTask | impl | (c) empty delivery cache is the console answer |
| bcat.c | 30201 | UnregisterBackgroundDeliveryTask | impl | (c) empty delivery cache is the console answer |
| bcat.c | 30202 | BlockDeliveryTask | impl | (c) empty delivery cache is the console answer |
| bcat.c | 30203 | UnblockDeliveryTask | impl | (c) empty delivery cache is the console answer |
| bcat.c | 90100 | EnumerateBackgroundDeliveryTask | impl | (c) empty delivery cache is the console answer |
| bcat.c | 90201 | ClearDeliveryCacheStorage | impl | (c) empty delivery cache is the console answer |
| bcat.c | 3 | CreateDeliveryCacheProgressService | impl | (c) empty delivery cache is the console answer |
| bcat.c | 4 | CreateDeliveryCacheProgressServiceWithApplicationId | impl | (c) empty delivery cache is the console answer |
| audio.c | 4 | QueryAudioDeviceSystemEvent | impl | (a) over libnapple_audio |
| audio.c | 11 | QueryAudioDeviceInputEvent | impl | (a) over libnapple_audio |
| audio.c | 12 | QueryAudioDeviceOutputEvent | impl | (a) over libnapple_audio |
| audio.c | 13 | GetAudioSystemMasterVolumeSetting | impl | (a) over libnapple_audio |
| audio.c | 11 | ExecuteAudioRendererRendering | impl | (a) over libnapple_audio |
| audio.c | 9 | GetAudioOutBufferCount | impl | (a) over libnapple_audio |
| audio.c | 10 | GetAudioOutPlayedSampleCount | impl | (a) over libnapple_audio |
| audio.c | 11 | FlushAudioOutBuffers | impl | (a) over libnapple_audio |
| audio.c | 1 | SetContext | impl | (a) over libnapple_audio |
| audio.c | 3 | SetContextForMultiStream | impl | (a) over libnapple_audio |
| psm.c | 0 | BindStateChangeEvent | impl | (a) events + supply count |
| psm.c | 1 | UnbindStateChangeEvent | impl | (a) events + supply count |
| psm.c | 2 | SetChargerTypeChangeEventEnabled | impl | (a) events + supply count |
| psm.c | 3 | SetPowerSupplyChangeEventEnabled | impl | (a) events + supply count |
| psm.c | 4 | SetBatteryVoltageStateChangeEventEnabled | impl | (a) events + supply count |
| psm.c | 5 | AcquireControllerPowerSupply | impl | (a) events + supply count |
| psm.c | 6 | ReleaseControllerPowerSupply | impl | (a) events + supply count |
| psm.c | 15 | GetBatteryAgePercentage | impl | (a) events + supply count |
| psm.c | 16 | GetBatteryChargeInfoEvent | impl | (a) events + supply count |
| lbl.c | 0 | SaveCurrentSetting | impl | (a) state |
| lbl.c | 15 | SetAmbientLightSensorValue | impl | (a) state |
| lbl.c | 16 | GetAmbientLightSensorValue | impl | (a) state |
| lbl.c | 17 | SetBrightnessReflectionDelayLevel | impl | (a) state |
| lbl.c | 18 | GetBrightnessReflectionDelayLevel | impl | (a) state |
| lbl.c | 19 | SetCurrentBrightnessMapping | impl | (a) state |
| lbl.c | 21 | SetCurrentAmbientLightSensorMapping | impl | (a) state |
| lbl.c | 23 | IsAmbientLightSensorAvailable | impl | (a) state |
| lbl.c | 26 | ApplyCurrentBrightnessSettingToBacklightForVrMode | impl | (a) state |
| settings.c | 4 | GetRegionCode | impl | (a) host-derived values |
| settings.c | 8 | GetQuestFlag | impl | (a) host-derived values |
| settings.c | 23 | GetColorSetId | impl | (a) host-derived values |
| settings.c | 62 | GetDeviceTimeZoneLocationName | impl | (a) host-derived values |
| apm.c | 2 | SetCpuOverclockEnabled | impl | (a) tables + flag |
| apm.c | 6 | IsCpuOverclockEnabled | impl | (a) tables + flag |
| pdm.c | 0 | QueryAppletEvent | impl | (c) no play log kept |
| pdm.c | 6 | QueryPlayEvent | impl | (c) no play log kept |
| pdm.c | 7 | GetAvailablePlayEventRange | impl | (c) no play log kept |
| pdm.c | 8 | QueryAccountEvent | impl | (c) no play log kept |
| pdm.c | 9 | QueryAccountPlayEvent | impl | (c) no play log kept |
| pdm.c | 10 | GetAvailableAccountPlayEventRange | impl | (c) no play log kept |
| misc.c | 8 | GetAddOnContentListChangedEvent | impl | (c) controls off / immutable DLC set |
| misc.c | 9 | GetAddOnContentLostErrorCode | impl | (c) controls off / immutable DLC set |
| misc.c | 10 | GetAddOnContentListChangedEventWithProcessId | impl | (c) controls off / immutable DLC set |
| misc.c | 11 | NotifyMountAddOnContent | impl | (c) controls off / immutable DLC set |
| misc.c | 12 | NotifyUnmountAddOnContent | impl | (c) controls off / immutable DLC set |
| misc.c | 13 | NotifyMountAddOnContentWithProcessId | impl | (c) controls off / immutable DLC set |
| misc.c | 14 | NotifyUnmountAddOnContentWithProcessId | impl | (c) controls off / immutable DLC set |
| misc.c | 50 | CheckAddOnContentMountStatus | impl | (c) controls off / immutable DLC set |
| misc.c | 1 | Initialize | impl | (c) controls off / immutable DLC set |
| misc.c | 1001 | CheckFreeCommunicationPermission | impl | (c) controls off / immutable DLC set |
| misc.c | 1031 | IsRestrictionEnabled | impl | (c) controls off / immutable DLC set |
| misc.c | 1032 | GetSafetyLevel | impl | (c) controls off / immutable DLC set |
| misc.c | 1035 | IsFreeCommunicationDefaultValue | impl | (c) controls off / immutable DLC set |
| misc.c | 1018 | IsRestrictionTemporaryUnlocked | impl | (c) controls off / immutable DLC set |
| misc.c | 1061 | ConfirmStereoVisionPermission | impl | (c) controls off / immutable DLC set |
| misc.c | 1062 | EndFreeCommunication | impl | (c) controls off / immutable DLC set |
| misc.c | 1451 | IsStereoVisionPermitted | impl | (c) controls off / immutable DLC set |
| olsc.c | 0 | GetTransferTaskCount | impl | (c) offline answer |
| olsc.c | 1 | ListTransferTasks | impl | (c) offline answer |
| olsc.c | 2 | GetTransferTaskListChangedEvent | impl | (c) offline answer |
| olsc.c | 0 | GetDataTransferState | impl | (c) offline answer |
| olsc.c | 1 | SynchronizeSaveData | impl | (c) offline answer |
| olsc.c | 2 | GetSyncCompletionEvent | impl | (c) offline answer |
| olsc.c | 0 | Initialize | impl | (c) offline answer |
| olsc.c | 10 | GetTransferTaskListController | impl | (c) offline answer |
| olsc.c | 11 | GetRemoteStorageController | impl | (c) offline answer |
| olsc.c | 12 | GetDataTransferPolicyInfo | impl | (c) offline answer |
| olsc.c | 13 | DeleteUserSaveDataInSyncArea | impl | (c) offline answer |
| olsc.c | 1 | GetSaveDataBackupSetting | impl | (c) offline answer |
| olsc.c | 2 | SetSaveDataBackupSettingEnabled | impl | (c) offline answer |
| olsc.c | 3 | SetCloudBackupSetting | impl | (c) offline answer |
| olsc.c | 4 | GetCloudBackupSetting | impl | (c) offline answer |
| olsc.c | 5 | UpdateCloudBackupSetting | impl | (c) offline answer |
| olsc.c | 6 | GetCloudBackupSettingEvent | impl | (c) offline answer |
| olsc.c | 7 | IsSaveDataBackupEnabled | impl | (c) offline answer |
| olsc.c | 8 | SetSaveDataBackupEnabled | impl | (c) offline answer |
| olsc.c | 9 | GetBackupSettingChangedEvent | impl | (c) offline answer |
| olsc.c | 14 | GetSyncAreaUsage | impl | (c) offline answer |
| olsc.c | 15 | ResetSyncArea | impl | (c) offline answer |
| olsc.c | 16 | GetSyncAreaStatus | impl | (c) offline answer |
| spsm.c | 1 | SleepSystemAndWaitAwake | impl | (c) host never sleeps |
| spsm.c | 2 | GetNotificationMessage | impl | (c) host never sleeps |
| spsm.c | 3 | SetAwake | impl | (c) host never sleeps |
| ovln.c | 0 | Send | impl | (c) counted and logged |
| ovln.c | 0 | OpenSender | impl | (c) counted and logged |
| ectx.c | 0 | SetResult | impl | (c) recorded |
| ectx.c | 1 | SetResultWithContext | impl | (c) recorded |
| fgm.c | 0 | Initialize | impl | (c) request state kept |
| fgm.c | 1 | Set | impl | (c) request state kept |
| fgm.c | 2 | Get | impl | (c) request state kept |
| fgm.c | 3 | Cancel | impl | (c) request state kept |
| fgm.c | 0 | Initialize | impl | (c) request state kept |
| ngc.c | 0 | GetContentVersion | impl | (c) |
| ngc.c | 1 | Check | stub (kept) | (d) needs profanity word lists (data drop) |
| ngc.c | 2 | Mask | stub (kept) | (d) needs profanity word lists (data drop) |
| ngc.c | 3 | Reload | impl | (c) |
| ns.c | 0 | ListApplicationRecord | stub (kept) | (a) needs the running title record from the napple-build manifest |
| ns.c | 1 | GenerateApplicationRecordCount | stub (kept) | (a) same |
| ns.c | 2 | GetApplicationRecordUpdateSystemEvent | stub (kept) | (a) same |
| ns.c | 400 | GetApplicationControlData | stub (kept) | (a) needs control.nacp location from napple-build |
| ns.c | 11 | CalculateApplicationOccupiedSize | stub (kept) | (a) needs install sizes |
| ns.c | 43 | CheckSdCardMountStatus | impl | (c) offline / no updates |
| ns.c | 0 | GetLaunchRequiredVersion | impl | (c) offline / no updates |
| ns.c | 1 | UpgradeLaunchRequiredVersion | impl | (c) offline / no updates |
| ns.c | 35 | UpdateVersionList | impl | (c) offline / no updates |
| ns.c | 36 | PushLaunchVersion | impl | (c) offline / no updates |
| ns.c | 37 | ListRequiredVersion | impl | (c) offline / no updates |
| ns.c | 1000 | PerformAutoUpdate | impl | (c) offline / no updates |
| ns.c | 0 | RequestLinkDevice | impl | (c) offline / no updates |
| ns.c | 1 | RequestCleanupAllPreInstalledApplications | impl | (c) offline / no updates |
| ns.c | 2 | RequestSyncRights | impl | (c) offline / no updates |
| ns.c | 3 | RequestUnlinkDevice | impl | (c) offline / no updates |
| ns.c | 701 | ClearTaskStatusList | impl | (c) offline / no updates |
| ns.c | 702 | RequestDownloadTaskList | impl | (c) offline / no updates |
| ns.c | 21 | GetApplicationContentPath | stub (kept) | (a) content path layout |
| ns.c | 23 | ResolveApplicationContentPath | stub (kept) | (a) content path layout |
| ns.c | 1200 | NeedsUpdateVulnerability | impl | (c) offline / no updates |
| ns.c | 1201 | UpdateSafeSystemVersionForDebug | impl | (c) offline / no updates |
| ncm.c | 3 | GetContentIdByType | stub (kept) | (a) CNMT content-info parse |
| ncm.c | 4 | ListContentInfo | stub (kept) | (a) CNMT parse |
| ncm.c | 14 | LookupOrphanContent | impl | (c) |
| ncm.c | 16 | HasContent | stub (kept) | (a) CNMT parse |
| ncm.c | 17 | ListContentMetaInfo | stub (kept) | (a) CNMT parse |
| ncm.c | 8 | GetPath | impl | (c) |
| ncm.c | 9 | GetPlaceHolderPath | impl | (c) |
| time.c | 1 | SetCurrentTime | impl | (c)/(a) host clock |
| time.c | 3 | SetSystemClockContext | impl | (c)/(a) host clock |
| time.c | 2 | GetTestOffset | impl | (c)/(a) host clock |
| time.c | 3 | SetTestOffset | impl | (c)/(a) host clock |
| time.c | 200 | GetRtcValue | impl | (c)/(a) host clock |
| time.c | 201 | IsRtcResetDetected | impl | (c)/(a) host clock |
| time.c | 204 | GetInternalOffset | impl | (c)/(a) host clock |
| time.c | 102 | GetStandardUserSystemClockInitialYear | impl | (c)/(a) host clock |
| audin.c | 10 | SetDeviceGain | impl | (a) gain state |
| audin.c | 11 | GetDeviceGain | impl | (a) gain state |
| btm.c | 0 | AcquireBleScanEvent | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 3 | StartBleScanForGeneral | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 4 | StopBleScanForGeneral | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 5 | GetBleScanResultsForGeneral | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 6 | StartBleScanForPaired | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 7 | StopBleScanForPaired | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 8 | StartBleScanForSmartDevice | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 9 | StopBleScanForSmartDevice | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 10 | GetBleScanResultsForSmartDevice | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 17 | AcquireBleConnectionEvent | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 18 | BleConnect | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 19 | BleOverrideConnection | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 20 | BleDisconnect | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| btm.c | 21 | BleGetConnectionState | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 3 | Initialize | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 4 | Finalize | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 5 | EnableExternalDevice | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 6 | GetExternalDeviceId | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 7 | SendCommandAsync | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 8 | GetSendCommandAsynceResult | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 9 | SetEventForSendCommandAsycResult | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 10 | GetSharedMemoryHandle | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 11 | EnableJoyPollingReceiveMode | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 12 | DisableJoyPollingReceiveMode | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 13 | GetPollingData | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| hidbus.c | 14 | SetStatusManagerType | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 3 | GetDisconnectReason | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 4 | GetSecurityParameter | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 5 | GetNetworkConfig | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 101 | GetNetworkInfoLatestUpdate | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 104 | SetWirelessControllerRestriction | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 205 | Reject | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 206 | SetAdvertiseData | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 207 | SetStationAcceptPolicy | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 208 | AddAcceptFilterEntry | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| ldn.c | 209 | ClearAcceptFilter | stub (kept) | (b)/other owner: not in this assignment (hardware simulation or another session) |
| vi.c | 0 | TransactParcel | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| vi.c | 1 | AdjustRefcount | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| vi.c | 2 | GetNativeHandle | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| vi.c | 3 | TransactParcelAuto | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| vi.c | 1101 | SetDisplayEnabled | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| vi.c | 2101 | SetLayerScalingMode | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 4 | QueryEvent | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 5 | MapSharedMemory | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 6 | GetStatus | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 7 | SetAruid | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 8 | DumpGraphicsMemoryInfo | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 9 | InitializeDevtools | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 12 | SetGraphicsFirmwareMemoryMarginEnabled | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
| nvdrv.c | 13 | Unknown13 | stub (kept) | (d) graphics/nvdrv path, owned by the gfx work |
