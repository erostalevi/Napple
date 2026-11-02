---
branch: claude/ipc-reference-audit-2026-11-02-r4
orchestration: ipc-reference-audit
base: ed33737
head: c474059
env: mac
date: 2026-11-02
areas: [runtime/services]
status: ready-with-caveats
---

# claude/ipc-reference-audit-2026-11-02-r4: misnumbered IPC rows follow switchbrew, nifm answers like an offline console, 17 more stubs real

## Summary
Of the 67 "is named X, the reference says Y" notes of `test_ipc_reference runtime -v`, 65 are resolved; the fixtures were right in every case (checked against the switchbrew tables), so only source rows changed (no fixture edited). Many were real wrong ids, not just names: pctl 1018/1035/1061/1062/1451, pdm:qry 1-10, ISteadyClock 100-102/200, bsd 28-31, ISslConnection 19-21, IGeneralService 15-20, fsp-srv 30, miiimg (whole table), olsc:u (guessed rows removed), ectx:aw, prepo:u, aoc:u 13; the rest are name-only. nifm `IRequest` GetResult / GetAppletInfo / GetRequirement / GetRevision answer like an offline console (GetAppletInfo of a failed request: netConnect library applet, id 0x0f, mode 0, empty argument array); GetAdditionalInfo stays a documented default (undocumented layout). 17 documented-default rows became real (IRequest 16-21, 23-25; IGeneralService 19, 20, 28, 34, 40, 41, 43, 57), plus new real rows (ISteadyClock 102, ISslConnection 20/21, pctl 1065, IContextRegistrar::Complete). NOT done: the two notes in `applet.c` (below) and the other rows of the assignment's list (fs.c / vi / btm / lm / ovln), which have undocumented layouts or no local data (reasons in the change note).

## Files
`git diff --stat ed33737..HEAD`: services `audio.c audrec.c bcat.c caps.c ectx.c fs.c irs.c ldn.c mii_img.c misc.c offline.c olsc.c pdm.c sfdnsres.c time.c`; tests `test_svc_{ectx,fs_dataid,ldn,local_offline,mii_img,offline,offline_depth,olsc,online,pctl,pdm,sysdef_offline,time}.c`, new `test_svc_nifm_audit.c`; `docs/architecture/runtime-services.md` (rows in place + one appended section); `docs/archive/changes/2026-11-02-claude-ipc-reference-audit-2026-11-02-r4.md`. No applet file, no fixture, no `test_ipc_reference.c` touched.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: mismatches resolved | tested (65 of 67; 2 left in applet.c) | `runtime/build/tests/test_ipc_reference runtime -v` now lists only `IApplicationFunctions 210` and the am `IStorage` (name clash with fsp-srv's) |
| Acceptance 2: nifm IRequest offline answers | tested | `test_svc_nifm_audit.c` (122 checks): GetResult before / after Submit / after Cancel, GetAppletInfo = {0x0f, 0, 4} + `-1` terminator, GetRequirement 0x24 bytes |
| Acceptance 3: >= 15 more local stubs real | tested (17) | list above; each has a check in `test_svc_nifm_audit.c` |
| Acceptance 4: `make -C runtime test test-services test-ipc-reference` | tested | rc 0, every suite `"failed":0`; `{"fixtures":111,"app_reachable":1325,"missing":0,"errors":0}` |
| Four-title 60 s boots | verified (no regression) | SMO 3ca12dfa, MK8D fe941ed5, LAS ae16f71e, ACNH ff1d1c05: each rc 0, `last_stage notify-running`, `missing_ipc []`, `unsupported_svcs []`, `stubbed_ipc` only `nvdrv:13`. Runtime binary, headless, private copied `--save-root`, under `.run.lock`. The logs only show which services were opened (pctl, nifm:u, ectx:aw, olsc:u ...), not which commands, so the renumbered rows stay *tested*, not *verified* |

## Unbuilt / untested list
Nothing unbuilt. Not called by any title that could be traced: every renumbered row. Inferred (not from a switchbrew layout): nifm GetAppletInfo applet choice + empty argument array, GetRequirement padding, wireless default ON / ethernet default OFF, the ectx registrar argument layout, miiimg migration rows (18-20) and prepo 10106/10107 sharing the 10104/10105 handler.

## Findings
- **Behaviour change to watch**: `IGeneralService` 17 IsWirelessCommunicationEnabled now reports the setting (default true); before, the id was served by a handler that effectively answered false. A title that branches on it would now try to connect (rejected by the NullProvider as before). No title of the four boots changed behaviour.
- olsc:u: ACNH 3.0.3 calls cmd 13 (switchbrew GetSaveDataBackupSetting) at boot and ends the process on an error; kept answering success. The guessed rows 1-9, 11, 12 were removed (not in switchbrew); the boot passed.
- `fsp-srv` 30 was a duplicate of 202; the documented id is OpenGameCardStorage (no card: default).
- Outside my area: `applet.c` `IApplicationFunctions` 210 name (`Cmd210`), and rename the am `IStorage` object (`applet.c:813`, e.g. `IStorageAm`) so the checker stops comparing it with fsp-srv's `IStorage`.
- Contract with am-stubs-legal: nifm GetAppletInfo returns AppletId 0x0f (netConnect, same id as `applet_lib_core.c`), LibraryAppletMode 0, argument array empty (`-1` terminator only), only for a failed request.

## How to integrate
No order constraints. Hotspots: `offline.c`, `misc.c` (aoc, pctl), `docs/architecture/runtime-services.md` (appended section). Build: `make -C runtime`; tests: `make -C runtime test test-services test-ipc-reference`.

## How to test
1. `make -C runtime test test-services test-ipc-reference` -> rc 0, `missing:0`.
2. `runtime/build/tests/test_ipc_reference runtime -v | grep "reference says"` -> two lines (applet.c).
3. Four-title boots: `lockf -k /Users/neto/NappleWork/.run.lock runtime/build/napple-runtime <data dir> --headless --seconds 60 --summary <json> --save-root <private copy>`; pass = rc 0, `missing_ipc []`.

## Fix policy hints
Safe: names, result-code choices, the wireless default. Needs a decision: replacing the inferred GetAppletInfo arguments once the netConnect applet shows what it reads.

## Needs from other areas
- am-stubs-legal / applet.c: the two items above.
- `docs/index.md`: link the change note (orphan like the other notes).
