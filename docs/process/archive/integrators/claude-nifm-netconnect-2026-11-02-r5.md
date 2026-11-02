---
branch: claude/nifm-netconnect-2026-11-02-r5
orchestration: nifm-netconnect
base: 2a5dab5 (orchestrate/2026-11-02-r5)
head: see `git log -1` (code at 5e7c1eb; docs commit on top)
env: mac
date: 2026-11-02
areas: [runtime/services/nifm, runtime/services/applet_lib_core, runtime/services/vi]
status: integrated
---

# claude/nifm-netconnect-2026-11-02-r5: nifm Cancel semantics + the real NetConnect output (MK8D Online Play shows the console's flow)

## Summary
- **Cancel:** nifm IRequest Cancel keeps a rejected request's result and applet info. Only an accepted request is released and reset.
- **GetAppletInfo:** its input list now carries the two storages netConnect reads: CommonArguments, then `{u32 0; Result; Uuid}`. The SDK pushes
  only what that list names.
- **NetConnect output:** now what the real applet writes, `{u32 0; Result}`. Offline the Result is 2110-2900 ("left without a connection").
- **Evidence, all from disassembly with addresses (in the change note):**
  - MK8D's nnSdk: `PrepareLibraryApplet`, `AppletToNifmArgumentsReader`, `NetworkConnection::HandleNetworkRequestResult`, the nn::la writers;
  - firmware 22.1.0 netConnect (0100000000001006), built privately: its output writer, its exit path and its input readers.
- **Why the old output looped:** `{2110-1111, 0}` read as Result 0. The SDK turns 0 into 2110-0190 ErrorHandlingCompleted, and MK8D resubmits on
  0190. That gave the ~20 retries.
- **Verified on MK8D update Online Play offline:** one `CreateLibraryApplet id 0xf` per attempt (two attempts, two applets), no retry loop, the
  game returns to the main menu, rc 0, `missing_ipc []`.
- **Filler:** vi 2102 ConvertScalingMode is real. The fs.c rows 15 / 16 / 606 / 618 were not touched: no layout was confirmed here.
- Not done: nothing from the acceptance list. The real netConnect applet was disassembled, not run: its UI needs input, and NAPPLE_FIRMWARE_APPLET_IDS
  for 0x0f was not tried.

## Files
- `runtime/src/services/offline_core.c` / `offline_core.h`: `nifm_request_cancel`.
- `runtime/src/services/offline.c`: the nifm part only, `nifm_get_applet_info`, plus `#include "../napple.h"` for `runtime_guest_ticks`.
- `runtime/src/services/applet_lib_core.[ch]`: NetConnect encoder and handler, `APPLET_NETCONNECT_R_CONNECTED` / `_NOT_CONNECTED`.
- `runtime/src/services/vi.c`: `ds_convert_scaling`, row 2102.
- Tests:
  - `runtime/tests/test_svc_nifm_audit.c`: the pin at the old line 47 was rewritten; a GetAppletInfo list case and an accepted-Cancel case were added.
  - `test_svc_applet_am.c`: the NetConnect cases.
  - `test_svc_offline.c`: two Cancel pins (it tests the same nifm request; the old expectation is now wrong).
  - `test_svc_vi.c`: ConvertScalingMode.
- Docs:
  - new `docs/archive/changes/2026-11-02-claude-nifm-netconnect-2026-11-02-r5.md`;
  - `docs/architecture/runtime-services.md`: an appended section, and the nifm table row corrected ("empty argument array" was wrong now).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: Cancel keeps a rejected request's result and applet info; an accepted one is released and reset; pin updated | tested + verified (MK8D) | `test_svc_nifm_audit`: Cancel after the rejection, then GetResult 2110-1111, GetAppletInfo names 0x0f, state Free; with an installed provider that connects, Submit gives Accepted, then Cancel releases once (`net_release`), state Free, GetResult 0, IsAnyInternetRequestAccepted false, a second Cancel releases nothing. MK8D: Submit, GetResult, Cancel, then 1.6 s later GetAppletInfo, and the applet is created |
| Acceptance 2: NetConnect output from evidence (addresses), and whether GetAppletInfo's list is filled | done (disassembly) | Change note "Evidence": SDK +0x150e84 / +0x155c44 / +0x1b5374 / +0x156ee4 / +0x1b58e8; netConnect +0x245d80 / +0x245da0 / +0x314cec / +0x245e40 / +0x245f80. Decision: the list **is** filled (the SDK pushes nothing else; netConnect reads CommonArguments and a 0x18-byte argument) |
| Acceptance 3: MK8D update Online Play offline | **verified** | `NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A NAPPLE_TRACE_IPC=all`, headless 55 s, private save copy: two attempts (23.2 s, 33.1 s), each with exactly one Submit, Cancel, GetAppletInfo, `CreateLibraryApplet id 0xf mode 0`, two PushInData, Start, GetResult, one PopOutData; no other Submit; screenshots: "Connecting...", then the main menu with Online Play selected; rc 0, `missing_ipc []`. Before: 20 applets in 5.5 s, then "A communication error has occurred." |
| Acceptance 4: `make -C runtime GFX=napple test test-services test-ipc-reference` | tested | rc 0; `ipc_reference` 111 fixtures, app_reachable 1325, missing 0; nifm_audit 144, applet_am 70, offline 70, vi 123, all 0 failed |
| Acceptance 5: four-title boots, SMO / ACNH nifm calls unchanged | verified | Headless, private save copies, run lock, no input. Head: SMO, MK8D, LAS 60 s and ACNH 90 s, all rc 0, `missing_ipc []`, `unsupported_svcs []`, stubbed only `nvdrv:13`. A base binary (same tree, base sources of the changed files) for SMO and ACNH: the nifm counters are identical (SMO `nifm:u:5, IGeneralService:4, IRequest 0 x2 / 1 x2 / 2 / 4`; ACNH the same plus `IRequest:11`). Neither calls Cancel or GetAppletInfo at boot. MK8D Online Play rechecked on the final binary: 2 attempts, 2 CreateLibraryApplet 0xf, 2 Submit, 2 PopOutData, rc 0 |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt.
- GetAppletInfo's "buffer too small" branch is unit-tested only (the SDK always passes 0x1000).
- The 0190 (connected) path is never produced by this host (no network to offer).

## Findings
- **Root cause of the retry loop:** the SDK reads netConnect's Result at offset 4, and a Result of 0 means "error handled, try again"
  (`NetworkConnection::HandleNetworkRequestResult` maps 0 to 2110-0190, and MK8D resubmits on 0190).
- **Decision:** offline, the HLE NetConnect answers 2110-2900, what netConnect writes when the user backs out without a connection (exit code
  nonzero). It pushes the 8-byte storage only. The firmware applet also pushes a 4-byte `{exit code}` storage first when a version field it holds
  is 5 or more; the SDK maps that one to the same Result.
- **Decision:** CommonArguments in GetAppletInfo's list has la version 0. The nifm sysmodule (010000000000000F, extracted privately) was searched for its
  packer without success (stripped, no SDK CommonArgumentsWriter in it), so the value is [unverified]. Only a firmware netConnect run would read it
  (the version-5 check above).
- `2110-0190` = ErrorHandlingCompleted and `2110-2900` = "not connected" are named from their use here. The SDK symbols do not name them.
- Outside my area: none found.
- Build pitfall (not a code bug): after swapping sources for a base build and back within the same second, `make` (1-second mtimes) kept the base objects. `touch` the sources before rebuilding.

## How to integrate
- No order constraints. Conflict hotspots:
  - `docs/architecture/runtime-services.md`: an appended section plus one table row; keep both sides.
  - `runtime/src/services/vi.c`: one row and one function above `ds_close_layer`. leftovers-a32-viewport-docs may touch vi; keep both.
- Build: `make -C runtime`, then `make -C runtime GFX=napple test test-services test-ipc-reference` (expect rc 0, missing 0).
- No Linux-only calls.

## How to test
1. `make -C runtime GFX=napple test test-services test-ipc-reference`. Pass:
   - `{"test":"svc_nifm_audit","checked":144,"failed":0}`;
   - `svc_applet_am` 70, `svc_offline` 70, `svc_vi` 123 at 0 failed;
   - `ipc_reference` missing 0.
2. MK8D update, headless 55 s with a copied save:
   `NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A NAPPLE_TRACE_IPC=all runtime/build/napple-runtime <games>/0100152000022000/fe941ed5ba14be5d --headless --seconds 55 --save-root <copy>`.
   - Pass: `grep -c "CreateLibraryApplet id 0xf"` gives 2, `grep -c "IRequest::Submit"` gives 2, rc 0.
   - Fail (old behaviour): about 20 CreateLibraryApplet in a row, or 0 (Cancel cleared the result).
3. Regression: the four-title boots (60 s, ACNH 90 s). Pass: rc 0, `missing_ipc []`.

## Fix policy hints
- Safe: test wording, include order.
- Needs a decision:
  - the la version in GetAppletInfo's CommonArguments, if a firmware netConnect run (NAPPLE_FIRMWARE_APPLET_IDS=0x0f) shows it matters;
  - whether to also push the 4-byte `{exit code}` storage the firmware pushes for version 5 or more (no SDK-visible difference).

## Needs from other areas
- None.

## Integration result
- Date 2026-11-02, merged into main as add77a1 (branch head 469e0f3: work 5e7c1eb + the integrator-file commit), first of batch r5; no conflicts. Outcome: **Clean**. No `integrate:` fixes, no native ports (pure service logic, nothing platform-specific).
- `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0; `svc_nifm_audit` 144/0, `svc_applet_am` 70/0, `svc_offline` 70/0, `svc_vi` 123/0; `ipc_reference` 111 fixtures, app_reachable 1325, missing 0.
- MK8D update Online Play offline (`NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A NAPPLE_TRACE_IPC=all`, headless 55 s, private save copy, run lock): rc 0, `missing_ipc []`, stubbed only `nvdrv:13`; two attempts (Submit 23.3 s / 33.2 s), each one Submit, Cancel, GetAppletInfo, `CreateLibraryApplet id 0xf mode 0`, PopOutData; 2 applets total, no retry loop. **verified**.
- Correction to "How to integrate": `make -C runtime` needs `audio`, `input`, `frontend` built and `make -C gfx adapter` (for `gfx/build/obj/adapter/gfx_napple.o`) in a fresh worktree; `make -C gfx` alone does not build the adapter object.
- Main checks after the whole batch (tree identical to main bcaa690 / abd1e70, built in a scratch worktree; run lock, private copied save roots, shared title data read-only): MK8D update Online Play offline (`NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A`, `NAPPLE_TRACE_IPC=all`, 45 s): one Submit / Cancel / GetAppletInfo / `CreateLibraryApplet id 0xf mode 0` / PopOutData at 23.4-25.0 s, no further Submit, the game keeps rendering to the end (2626 frames), rc 0, `missing_ipc []`. Four-title boots with `MTL_DEBUG_LAYER=1` (A at 25/40/55/70 s; 60 s, ACNH 90 s): SMO, MK8D, LAS, ACNH all rc 0, `missing_ipc []`, `unsupported_svcs []`, stubbed only `nvdrv:13`, 0 skipped draws, 0 unimplemented calls, 0 warnings, no validation errors (banner only); frames SMO 2709 / MK8D 3390 / LAS 3459 / ACNH 2564 on a cold shader cache (0 from disk cache: `TRANSLATOR_VERSION` 19 invalidated it) with another batch's session sharing the machine, so below the warm r4 numbers as the leftovers author found for cold runs. `make -C runtime GFX=napple test test-services test-ipc-reference test-host` rc 0 (nifm_audit 144, applet_am 70, offline 70, vi 123, kernel_exit 19, all 0 failed; ipc_reference missing 0).
