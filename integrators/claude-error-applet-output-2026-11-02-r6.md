---
branch: claude/error-applet-output-2026-11-02-r6
orchestration: error-applet-output
base: 7ea30c3 (orchestrate/2026-11-02-r6)
head: 4311917 (code; the commit on top adds the change note and this file)
env: mac
date: 2026-11-02
areas: [runtime/services/applet]
status: ready
---

# claude/error-applet-output-2026-11-02-r6: empty applet channels answer what the SDK expects; error applet modes decoded

## Summary
- **Root cause of MK8D's Online Play abort with `NAPPLE_NET=host`:**
  - our am answered PopOutData on an empty channel with 0x680 (NoMessages);
  - the SDK's `nn::applet::TryPopFromOutChannel` accepts only 0x480 (NoDataInChannel) as "empty" and aborts on anything else;
  - after the error applet the SDK always tries one pop, so every error applet ended in svcBreak.
  - The error applet itself needs no output: the SDK reads an optional 2-byte storage, byte 1 being the jump destination.
- **Fix:**
  - every empty storage channel answers 0x480 (Pop(Interactive)OutData, Pop(Interactive)InData, the TryPopFrom*Channel commands);
  - unimplemented library applets end with 0x2c80 (LibraryAppletTerminated, exit reason Canceled) instead of 0x1480, which the SDK maps to
    "Unexpected";
  - the error applet decodes the startup parameter of every mode (normal, system, application, EULA, result, error record, recorded, system
    update EULA) for its log line, the dialog hook and the run summary;
  - the evidence (MK8D sdk addresses) is in the change note.
- **Verified:** MK8D Online Play with `NAPPLE_NET=host` on this branch merged with net-host-provider (scratch worktree, removed). The error applet
  shows (2001-0123) and the game returns to its Online Play menu, retries, and returns again. rc 0, no Break.
- **Audit:**
  - every other library applet pushes its output before a normal exit;
  - the error applet is the only one without output, which the SDK accepts;
  - nothing else is missing.

## Files
`git diff --stat 7ea30c3..HEAD`:
- `runtime/src/services/applet_core.h` (three result defines with the evidence), `applet.c` (PopOutData / PopInteractiveOutData / friend-invitation
  and notification channels, the unimplemented-applet result, `libapplet_run_error` storage choice and comment), `applet_ae.c` (PopInData,
  PopInteractiveInData, TryPopFromAppletBoundChannel), `applet_lib_core.[ch]` (`AppletErrorInfo` extended, `applet_error_parse` per mode,
  `applet_error_type_name`, describe / code text).
- Tests: `runtime/tests/test_svc_applet.c`, `test_svc_applet_ae.c`, `test_svc_applet_host.c`, `test_svc_applet_libs.c`.
- `docs/archive/changes/2026-11-02-claude-error-applet-output-2026-11-02-r6.md` (new), this file.
- Not touched: `runtime/src/net/`, `offline*.c`, `online_core.c`, `account*.c`, `crates/`, `gfx/`, `orchestrations/`. `runtime/tests/fuzz/t_libapplet.c`
  compiles unchanged (the `applet_error_parse` signature is kept).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. Error applet modes push the output / exit the SDK expects (evidence + unit tests) | tested | Disassembly of MK8D v1376256 sdk: nn::err waiter +0x11b1c8 (TryPopFromOutChannel, 2-byte read, byte 1 returned), TryPopFromOutChannel +0xd316c (`cmp w8, #0x480`, else AbortImpl), JoinLibraryAppletImpl +0xd2ddc (GetResult -> exit reason), startup parameters of every entry point +0x11a51c..+0x11b2e4 (table in the change note). `test_svc_applet_libs` 290/0: every mode's parameter, ShowError(Result) through the accessor (exit 0, no output, PopOutData 0x480 twice), unimplemented applet 0x2c80 |
| 2. MK8D update Online Play with `NAPPLE_NET=host` returns to its menu, no svcBreak, rc 0 | **verified** | Scratch worktree = this branch + `claude/net-host-provider-2026-11-02-r6` (clean merge), full build, run lock, private copy of the shared MK8D save. `NAPPLE_NET=host NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A ... --seconds 55`: rc 0; `error applet: type 0 (normal), jump-to-settings requested (not done), ..., code 2001-0123` -> `PopOutData answered with error result 0x480` -> Online Play menu at 27 / 30 s; A at 32 s -> second error applet -> menu at 42 / 50 s. Summary partial (time limit), `missing_ipc []`, `unsupported_svcs []`. Evidence: shared notes `error-applet-output-evidence/` (montage, trace, summary); logs `/Users/neto/NappleWork/logs/error-applet-r6-mk8d-online/` |
| 3. Other library applets audited | done | swkbd (normal / inline), controller, player select, netConnect, web / offline web, photo viewer, firmware Mii Maker push output before a normal exit; the unimplemented-applet path now ends Canceled. Offline Online Play on the scratch merge: netConnect twice, rc 0 (unchanged) |
| 4. `make -C runtime test test-services test-ipc-reference` | tested | rc 0 (`ipc_reference` 111 fixtures, missing 0); applet suites 116 / 52 / 290 / 539 / 67 / 219 / 63, 0 failed |
| 4. Four-title boots no regression | verified | This branch's runtime, `MTL_DEBUG_LAYER=1`, 60 s (ACNH 90 s), A at 25 / 40 / 55 / 70 s, run lock, cold save roots: all exit 0, 0 skipped draws, 0 warnings, no validation error; frames 2602 / 3551 / 2724 / 3462 (ACNH / LAS / SMO / MK8D), cold Metal compiler cache like r5's first run (2612 / 3552 / 2603 / 3440). Logs `/Users/neto/NappleWork/logs/error-applet-r6-head-<titleid>/` |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt.
- Error modes 1-8 were tested with synthetic parameters, not seen from a title. MK8D's path is type 0 with an ErrorCode.
- The friend-invitation / notification / applet-bound channel results follow the SDK's storage-channel readers by analogy. MK8D's SDK has no reader
  for those three commands.

## Findings
- The 0x680 vs 0x480 confusion affected more than the error applet. Any title that pops a storage channel once more than it was given aborted:
  - a game reading the optional output of any applet;
  - a firmware applet reading past its last input.
- 0x1480 (description 10) as the "cancelled" GetResult was read by the SDK as exit reason Unexpected. 0x2c80 is the description it maps to Canceled.
- MK8D shows its error through `ShowError(ErrorCode)` (2001-0123, from `EnsureIdTokenCacheAsync`'s 0xf601) with the jump flag set. A console would
  offer "go to System Settings". The headless answer dismisses it, so no jump, and the game goes back to the menu by itself.
- `ExecuteJump` in this SDK is a 4-byte stub (`nn::err::ExecuteJump` +0x11aecc), so even a returned jump destination would not move the title.
- This branch's base has no nifm-netconnect: MK8D offline (no `NAPPLE_NET`) never launches the netConnect applet here; it does once merged.

## How to integrate
- No ordering constraint against net-host-provider: the two merge cleanly (`applet_lib_core.[ch]` auto-merged in the scratch check).
  Land both for the Online Play path.
- Conflict hotspots: `runtime/src/services/applet.c` (other branches touching applet rows: keep both); tests that pin 0x680 / 0x1480 for an
  empty channel or an unimplemented applet in other branches must move to 0x480 / 0x2c80.
- Docs: this change note needs a row in `docs/archive/changes/README.md` ("Unattended rounds" section from r5, not in this base) or `check_docs.py`
  reports it as an orphan.
- Build: `make -C runtime GFX=napple`. No Linux-only calls.

## How to test
1. `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0; `build/tests/test_svc_applet_libs` -> `{"test":"svc_applet_libs","checked":290,"failed":0}`.
2. With net-host-provider merged: `NAPPLE_NET=host NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A NAPPLE_TRACE_IPC=all runtime/build/napple-runtime <games>/0100152000022000/fe941ed5ba14be5d --headless --seconds 55 --save-root <copy of the MK8D save>` -> rc 0, `PopOutData answered with error result 0x480` after each `error applet:` line, no `guest Break`. Failing looks like `PopOutData ... 0x680` then `SetTerminateResult` and `guest Break` (rc 70).
3. Four-title boots: exit 0, 0 skipped draws.

## Fix policy hints
- Result constants and test expectations are safe to adjust directly if a console trace shows otherwise.
- Pushing a real error-applet output (2 bytes `{0, jump destination}`) would be a design choice for a frontend that offers "go to settings". The SDK
  accepts both forms; headless keeps none.

## Needs from other areas
- `docs/architecture/runtime-services.md` (applet section, not mine): empty channels answer NoDataInChannel 0x480 (the message queue 0x680);
  unimplemented library applets end LibraryAppletTerminated 0x2c80; the error applet modes table (change note).
- account (`account.c`): `EnsureIdTokenCacheAsync` answers 0xf601 synchronously. The game handles it, but net-host-provider noted that a console
  answers through an IAsyncContext.
- `.claude/skills/percent-analysis/verified.json`: the error applet row can cite this MK8D run (verified with `NAPPLE_NET=host`).
