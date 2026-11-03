# 2026-11-02 error-applet-output (batch 2026-11-02-r6, Mac)

Branch `error-applet-output-2026-11-02-r6` (base `batch/2026-11-02-r6` = `7ea30c3`). net-host-provider found that MK8D's Online Play with
`NAPPLE_NET=host` aborts (svcBreak, rc 70) right after the error applet. The cause is not a missing error-applet output. It is the result our am
answers when a storage channel is empty.

## Evidence (MK8D v1376256 `sdk`, module offsets; disassembled from the shared title data's `sdk.patched.bin`, imports resolved through `sdk.tables`)
- `nn::err::ShowError(Result)` (+0x11a51c) builds a 0x14-byte startup parameter and calls the launcher (+0x11b0f8: `CreateLibraryApplet(0xe)`,
  CommonArguments, `CreateStorage` / `WriteToStorage` / `PushToInChannel`), then the waiter (+0x11b1c8): `StartLibraryApplet`, `JoinLibraryApplet`,
  `GetLibraryAppletExitReason`. For a normal exit it calls `TryPopFromOutChannel`. With a storage it reads 2 bytes and returns byte 1 (the jump
  destination); without one it returns 0. Every nn::err entry point goes through this waiter. Only `ShowErrorWithoutJump` (+0x11a994, +0x11aa2c)
  keeps the returned value.
- `nn::applet::TryPopFromOutChannel` (+0xd316c) calls PopOutData and maps the result: 0 is a storage, `(r & 0x3fffff) == 0x480` (am NoDataInChannel,
  module 128 description 2) is "empty", and anything else goes to `nn::diag::detail::AbortImpl`. The other storage-channel readers compare the same
  0x480:
  - `TryPopFromInteractiveOutChannel` +0xd3210, `TryPopFromInChannel` +0xd384c, `TryPopFromInteractiveInChannel` +0xd38f0;
  - `TryPopFromApplicationParameterChannel` +0xd475c, `TryPopFromContextStack` +0xd3ce4;
  - `nn::la::PopFromOutChannel` (+0x1b47dc) is built on TryPopFromOutChannel.
  - Only the message queue uses 0x680 (NoMessages): `nn::oe::TryPopNotificationMessage` +0x15c214.
- Our PopOutData answered 0x680 on an empty channel. Hence net-host-provider's trace: PopOutData 0x680 -> the SDK's abort -> SetTerminateResult
  0x680 -> svcBreak.
- `nn::applet::JoinLibraryAppletImpl` (+0xd2ddc) turns GetResult into the exit reason the readers switch on:
  - 0 -> Normal;
  - module 128 description 22 (0x2c80) -> Canceled;
  - descriptions 20..49 -> Abnormal;
  - anything else -> Unexpected.
- Startup parameter of each mode (u8 type at +0, u8 jump flag at +1; +5 a ResultBacktrace storage follows, +6 the argument is an ErrorCode, +7 an
  ErrorContext storage follows):

| type | SDK entry | size | contents |
|---|---|---|---|
| 0 normal | ShowError(Result / ErrorCode), ShowErrorWithoutJump, ShowUnacceptable*VersionError, ShowNetworkServiceError | 0x14 | ErrorCode {category, number} at +8 when +6, else Result at +0x10 |
| 1 system | ShowSystemError (SystemErrorArg ctor +0x11b720) | 0x1018 | ErrorCode at +8, language +0x10, dialog message +0x18, full-screen +0x818 |
| 2 application | ShowApplicationError (ApplicationErrorArg ctor +0x11b2c4) | 0x1014 | number at +8, language +0xc, dialog message +0x14, full-screen +0x814 |
| 3 EULA | ShowEula | 0xc | region at +8 |
| 4 result | ShowError(Result) for module 142 descriptions 100..119 | 0xc | Result at +8 |
| 5 error record | ShowErrorRecord | 0x18 | ErrorCode at +8 (from a Result: 2000 + module, description), posix time at +0x10 |
| 6 recorded error | CreateErrorViewerStartupParamForRecordedError | variable | not decoded |
| 8 system update EULA | ShowSystemUpdateEula | 0xc + 0x20000 storage | region at +8 |

## Changes (`runtime/src/services/`)
- `applet_core.h`: `APPLET_R_NO_DATA_IN_CHANNEL` (0x480), `APPLET_R_NO_MESSAGES` (0x680), `APPLET_R_LIBRARY_APPLET_TERMINATED` (0x2c80), with the
  evidence above.
- Empty storage channels answer 0x480 (they answered 0x680):
  - `ILibraryAppletAccessor` PopOutData / PopInteractiveOutData (`applet.c`);
  - `IApplicationFunctions` TryPopFromFriendInvitationStorageChannel / TryPopFromNotificationStorageChannel (`applet.c`);
  - `ILibraryAppletSelfAccessor` PopInData / PopInteractiveInData and `IAppletCommonFunctions` TryPopFromAppletBoundChannel (`applet_ae.c`, the
    library-applet side of a firmware applet run).
  - PopLaunchParameter already did. ReceiveMessage keeps 0x680, and so does ReadThemeStorage, which is not a channel.
- Unimplemented library applets (and a firmware applet that is turned off or produces nothing) end with GetResult 0x2c80 (exit reason Canceled),
  not 0x1480 (description 10: exit reason Unexpected, which the SDK readers abort on). The log line already said "reporting it as cancelled".
- Error applet (`applet_lib_core.[ch]`, `libapplet_run_error`):
  - `applet_error_parse` decodes the startup parameter of every mode in the table: code, Result or application error number; dialog message;
    region; context / backtrace flags.
  - The storage after it is used only for a parameter shorter than its type's layout (the earlier libnx-style split form).
  - The applet takes storage 1 as the parameter (storage 0 is CommonArguments).
  - The answer stays "dismissed, no jump": exit normally, no output. The SDK then reads an empty channel and returns no jump destination.
- Audit of the other library applets' outputs:
  - swkbd (normal and inline), controller support, player select, netConnect, web / offline web, photo viewer and the firmware Mii Maker all push
    their output storage before finishing normally;
  - the error applet is the only normal exit without output, and the SDK accepts that now;
  - a game that pops once more than the applet pushed now gets the empty-channel answer the SDK expects, instead of an abort.

## Verified here (Mac)
| Check | Result |
|---|---|
| applet unit tests | `test_svc_applet` 116/0, `_ae` 52/0, `_libs` 290/0 (startup parameter of every mode, PopOutData 0x480 after the error applet, a second PopOutData still 0x480, unimplemented applet 0x2c80), `_host` 539/0, `_am` 67/0, `_web` 219/0, `_album` 63/0 |
| `make -C runtime GFX=napple test test-services test-ipc-reference` | rc 0 (`ipc_reference` 111 fixtures, missing 0) |
| MK8D v1376256, Online Play, `NAPPLE_NET=host`, runtime built from this branch merged with `net-host-provider-2026-11-02-r6` (scratch worktree, check only) | rc 0, no Break; `EnsureIdTokenCacheAsync` 0xf601 -> error applet `type 0 (normal), jump-to-settings requested, code 2001-0123` -> PopOutData 0x480 -> the game back on its Online Play menu (screenshots at 27 s and 30 s); the scripted A at 32 s retries Online Play, a second error applet, the menu again (42 s, 50 s). Summary `partial`, "stopped by the run time limit", `missing_ipc []`, `unsupported_svcs []` |

Other checks:
- **Offline Online Play, scratch merge:** the same script without `NAPPLE_NET`. rc 0, the netConnect applet twice (a registered handler with
  output, PopOutData returns its storage), as before.
- **Offline, this branch alone:** its base has no nifm-netconnect, so `GetAppletInfo` refuses and no applet is launched. rc 0.
- Logs: `/Users/neto/NappleWork/logs/error-applet-r6-mk8d-online{,-offline,-offline-scratch}/`. Evidence copied to the shared notes folder
  `error-applet-output-evidence/`.

Four-title boots with this branch's runtime: `MTL_DEBUG_LAYER=1`, r5's settings (60 s, ACNH 90 s, A at 25 / 40 / 55 / 70 s), run lock, cold save
roots. Logs: `/Users/neto/NappleWork/logs/error-applet-r6-head-<titleid>/`.
- All exit 0, 0 skipped draws, 0 warnings, no Metal validation error.
- Frames ACNH 2602 / LAS 3551 / SMO 2724 / MK8D 3462. The Metal compiler cache was cold (11-33 ms per shader), the condition of r5's first run:
  2612 / 3552 / 2603 / 3440.
- No title raised a library applet in these scenes.

## Files
`runtime/src/services/applet.c`, `applet_ae.c`, `applet_core.h`, `applet_lib_core.c`, `applet_lib_core.h`; `runtime/tests/test_svc_applet.c`,
`test_svc_applet_ae.c`, `test_svc_applet_host.c`, `test_svc_applet_libs.c`; this note.
