# Verification prompt: services-2026-10-24 (run this on the Mac)

You are a fresh session on the user's Mac. You have no memory of how this branch was written. Your job is to build it, run the tests, run two games, and
report honestly what works. The branch was written on a Linux machine where `runtime/` does not build, so **nothing on it has ever been compiled by
Apple clang or run on macOS**. Do not assume anything passes; do not report anything as verified unless you ran it and saw the evidence.

Repository: `NappleCloudRepo`, the `Napple/` directory is the project root (all paths below are relative to it). Do not push. Do not merge into other branches.

## 1. Setup

1. Get the branch. It exists only in the machine that wrote it (it was not pushed). If `git branch --list services-2026-10-24` is empty, ask the user for the
   bundle file and run `git fetch <bundle-file> services-2026-10-24:services-2026-10-24`.
2. Base commit is `1f4b902` (the tip of the previous work). `git checkout services-2026-10-24`. The last commit that changes code is **`ced49f4`**; every
   later commit is documentation. Check: `git diff --stat ced49f4..HEAD` must list only `Napple/docs/...` files and `Napple/tools/linux-check/README.md`. Note the tip with `git rev-parse --short HEAD`
   and put it in your report.
3. Only `Napple/runtime/src/services/`, `Napple/runtime/tests/`, one appended line in `Napple/runtime/Makefile`, `Napple/tools/linux-check/` and `Napple/docs/`
   should differ from the base: `git diff --stat 1f4b902..HEAD -- . ':!docs' ':!tools/linux-check' ':!runtime/tests'`. Anything else (kernel, gfx, audio, `fs.c`,
   `nvdrv.c`) means the branch is not what this prompt describes: stop and report.
4. Build prerequisites (these libraries are not in git): `make -C input && make -C audio && make -C frontend && make -C gfx`. Expected: each ends without error.
   (If `make -C runtime` later complains that `../gfx/build/obj/adapter/gfx_napple.o` is missing, look at `gfx/Makefile` for the target that builds the adapter.)
5. Build the runtime: `make -C runtime 2>&1 | tee /tmp/runtime-build.log`. Expected: it links `runtime/build/napple-runtime`. Then list warnings that come from
   the files this branch added or changed: `grep -E "warning|error" /tmp/runtime-build.log | grep -E "services/(psm|lbl|mii|caps|ncm|ns|sfdnsres|spsm|bcat|ldn|irs|audin|hidbus|pdm|setfd|olsc|ectx|fgm|btm|ovln|time|applet|hid|offline)"`.
   Errors are expected possibilities (see section 5). Warnings that were already present on the base are not interesting; new ones in these files are.
6. Tests: `make -C runtime test` and `make -C runtime test-services`.
   - `make -C runtime test` runs the four pre-existing binaries (`test_ipc`, `test_fs_journal`, `test_hid_service`, `test_timescale`). Expected: each prints a JSON line with `"failed":0`, exit 0.
   - `make -C runtime test-services` builds and runs 25 binaries: `build/tests/test_svc_{applet,audin,bcat,btm,caps,ectx,fgm,hidbus,hidcore,irs,lbl,ldn,mii,ncm,ns,offline,olsc,ovln,pdm,psm,setfd,sfdnsres,spsm,time}` and `build/tests/test_hid_depth`.
     Expected: every one prints `{"test":...,"checked":N,"failed":0}`. The `checked` counts differ from Linux on purpose (a few Linux-only cases are compiled out);
     only `"failed":0` and exit code 0 matter. The Linux counts were: applet 63, audin 63, bcat 28, btm 12, caps 39, ectx 6, fgm 15, hidbus 17, hidcore 12, irs 19, lbl 42, ldn 38, mii 39,
     ncm 18, ns 58, offline 70, olsc 13, ovln 6, pdm 16, psm 37, setfd 32, sfdnsres 15, spsm 8, time 72. caps, ns and pdm have Linux-only cases, so their Mac counts are lower.
   - `test_hid_depth` was never compiled anywhere except a syntax-only pass; it is the most likely to need a fix.

## 2. What was never built or run on macOS (be honest about each in the report)

Everything below is unbuilt and unrun on macOS. "Linux-tested" means its portable logic ran in unit tests against a mock IPC layer on Linux; it does **not** mean it works against
the real `ipc.c`, the real handle/event code, or a game.

| Item | State before you started |
|---|---|
| New services in `runtime/src/services/`: `psm`, `lbl`, `mii`, `caps`, `ncm`, `ns` (ns:am2, ns:ec, ns:vm), `sfdnsres`, `spsm`, `bcat`, `ldn`, `irs`, `audin`, `hidbus`, `pdm`, `setfd` (set:fd), `olsc`, `ectx`, `fgm` (also registers `fgm:0`, `fgm:9`), `btm`, `ovln` (+ their `*_core.[ch]`, `hz_result.h`, `svc_common.h`) | Linux unit-tested, gcc+clang `-fsyntax-only`, never compiled for macOS, never reached by a game |
| `time.c` + `time_core.c` (LoadTimeZoneRule, rule-aware ToCalendarTime/ToPosixTime, ClockSnapshot 400/401/500/501, location from `/etc/localtime`) | same; **changes behaviour of an existing service** |
| `applet.c` + `applet_core.c` (~30 setters/getters now stateful) | same; changes an existing service; `caps:su` reads `applet_screenshot_permitted()` from it |
| `offline.c` + `offline_core.c` (bsd descriptor table and errnos, nifm request lifecycle, ssl handshake, friend Pop) | same; changes an existing service |
| `hid.c` + `hid_core.c` (six-axis, LED pattern, hold type, vibration permission, home protection, styles, 126/127/130/133-135/211/1000/1001) | `hid_core.c` Linux-tested; **`hid.c` itself could not even be compiled on Linux** (Mach + input library), only syntax-checked with stub headers; its handlers are tested only by `tests/test_hid_depth.c`, which has never run |
| `runtime/Makefile` (one appended line) and `runtime/tests/services.mk` (the `test-services` target) | never run |
| `runtime/tests/test_svc_*.c`, `svc_test.h`, `test_hid_depth.c` | the `test_svc_*` ran on Linux against `tools/linux-check/ipc_mock.c`; on macOS they link the real runtime objects and have never been built |
| `tools/linux-check/*` | Linux-only tooling; not part of your checks (you may ignore it) |

Facts that are **unverified against any console or title**: every IPC command id and argument layout of the new services (written from memory of public documentation), the
result descriptions marked `[unverified]` in `runtime/src/services/hz_result.h`, and the list of services each game's NPDM allows. FreeBSD errno numbers (bsd) and the
ClockSnapshot / Mii CharInfo layouts are the author's most confident parts, but still unverified.

Behaviour changes to existing services that can show up in a game (look for these specifically):
1. `bsd:u` `Socket` used to always fail (ENETDOWN); it now returns a descriptor and later calls answer per descriptor.
2. `nifm` `IRequest::GetResult` after `Submit` now returns an error (communication disabled); before it always succeeded.
3. `time`: `LoadTimeZoneRule` used to be an unknown command (it would have stopped the runtime); the device location name used to be `UTC`, now the Mac's zone (e.g. `Europe/Berlin`); `IsStandardUserSystemClockAutomaticCorrectionEnabled` and `IsStandardNetworkSystemClockAccuracySufficient` now return true.
4. `hid`: `PermitVibration(false)` now silences rumble; `GetSupportedNpadStyleSet` returns what the game set; `GetPlayerLedPattern` values changed from a constant.
5. `appletOE`: values the game sets are now returned by the matching getters (master volume, auto sleep, idle detection, ...).
6. `friend` notification `Pop` on an empty queue returns an error; `ssl` `DoHandshake` returns an error.

## 3. Checks, in order

Stop at the first failure that needs a design decision (see section 5). Otherwise continue and report every item.

**C1. Build (setup step 5).** Pass: `napple-runtime` is produced. Fail: any compile or link error. Fix typos/build errors per section 5, one commit each.

**C2. Pre-existing unit tests.** `make -C runtime test`. Pass: four JSON lines with `"failed":0`. Fail: any failure here is a regression from this branch (`hid.c` and
`applet.c` were edited; `test_hid_service` exercises `hid.c`).

**C3. New unit tests.** `make -C runtime test-services`. Pass: 25 binaries, all `"failed":0`. Fail: a build/link failure (common causes: a test calls `event_create`/`handle_new`
before kernel tables are initialised, or `guest_ptr` rejects a stack/static buffer address; the fix is then in the *test*, not in the service) or a failing CHECK (the message
names the line and what it expected; decide whether the service or the expectation is wrong; if the expectation came from a Linux-mock assumption, fix the test).

**C4. Which new services do the games ask for?** The service manager refuses services outside the NPDM list, so a service is only exercised if the game lists it.
Find the data directories (`ls work/games`): Link's Awakening is `01006BB00C6F0000`, Super Mario Odyssey is `0100000000010000`. For each, take the newest build directory
`D=$(ls -dt work/games/<titleid>/*/ | head -1)` and list the candidate services:
`strings -a -n 2 "$D/main.npdm" | grep -x -E '(mii:u|caps:su|caps:u|psm|lbl|ncm|ns:am2|ns:ec|ns:vm|sfdnsres|spsm|bcat:u|ldn:u|irs|audin:u|hidbus|pdm:qry|set:fd|olsc:u|ectx:aw|fgm|btm:u|ovln:snd|time:u|time:s|time:a|bsd:u|nifm:u|ssl|friend:u|hid|appletOE)'`
(NPDM service names are length-prefixed; if a name you expect is missing, cross-check with `grep -a -c '<name>' "$D/main.npdm"` before concluding it is absent).
Record, per game, which of the new services are listed. A service that neither game lists can only be reported as "unit-tested only".

**C5. Baseline run (before judging the branch).** Build the base in a separate worktree and run the same headless boot, so you compare like with like:
```
git worktree add /tmp/napple-base 1f4b902
( cd /tmp/napple-base/Napple && make -C input && make -C audio && make -C frontend && make -C gfx && make -C runtime )
mkdir -p /tmp/verify
for T in 01006BB00C6F0000 0100000000010000; do
  D=$(ls -dt work/games/$T/*/ | head -1); D=${D%/}
  NAPPLE_TRACE_IPC=1 NAPPLE_VERBOSE=1 NAPPLE_INPUT_LOG=1 /tmp/napple-base/Napple/runtime/build/napple-runtime "$D" --headless --seconds 45 \
    --trace /tmp/verify/$T-base.trace.jsonl --summary /tmp/verify/$T-base.summary.json --save-root /tmp/verify/saves-base > /tmp/verify/$T-base.log 2>&1
  echo "$T base exit $?"
done
```
(The runtime is single-GPU: do not run two game processes at once; `tools/dev_README.md` describes the `work/gpu.lock` convention.) If a game needs prepared data you do
not have, say so and mark its rows "not run". Keep the base logs; they are your reference.

**C6. Headless boot on the branch, same commands with the branch binary** (`runtime/build/napple-runtime`, outputs `-branch`, `--save-root /tmp/verify/saves-branch`), plus
`NAPPLE_GFX_CMDTRACE=1` for the graphics regression guard. Scripted input to prove the input path: add `NAPPLE_INPUT_SCRIPT="10:A,12:PLUS:0.3,30:L+R:0.8"`.
Pass, per game, all of:
 - exit code and last boot stage (`grep -E '"stage"|"status"|"reason"' summary.json`) equal to the base, or better;
 - `missing_ipc` in the summary has no entry that the base did not have (diff the two lists);
 - no new `runtime_fail` / `IPC argument out of range` / crash lines (`grep -E "runtime_fail|beyond the .* request bytes|SIGSEGV|crash" log`) compared with the base;
 - the input log shows the scripted presses (`NAPPLE_INPUT_LOG` lines) as on the base.
Fail: any new `missing_ipc`, `argument ... beyond the ... request bytes` (a wrong argument layout in a new/changed handler: that is a real bug), crash, hang, or an earlier boot stage.
A new `command N not implemented` line for an interface of this branch is not a crash but a finding: record the service, command id, and request size, and note it as a gap.

**C7. Were the features hit?** From the branch run with `NAPPLE_TRACE_IPC=1` (lines look like `ipc: <iface>::<Name> (cmd N) in=...`), count calls per new or changed interface:
`grep -E "ipc: (mii:u|IDatabaseService|caps:s?u|IAlbum|psm|IPsmSession|lbl|ncm|IContent(MetaDatabase|Storage)|ns:|IApplication(Manager|Version)Interface|IContentManagementInterface|IECommerce|sfdnsres|spsm|bcat|IBcat|IDeliveryCache|ldn|IUserLocalCommunicationService|irs|audin|IAudioIn|hidbus|pdm|set:fd|IFirmwareDebug|olsc|ectx|fgm|IRequest|btm|IBtmUserCore|ovln|ISender|ITimeZoneService|time:|ISelfController|IAudioController|ICommonStateGetter|IApplicationFunctions|bsd:u|nifm|ssl|friend|hid)" /tmp/verify/<T>-branch.log | sed 's/ in=.*//' | sort | uniq -c | sort -rn | head -80`.
For each changed behaviour in section 2 (list items 1-6), record the exact log lines that show it was or was not exercised. "Not exercised" is a valid, honest result and means the item stays unverified.
Useful switches: `NAPPLE_BATTERY="42,battery"` (psm; only matters if a game calls psm), `NAPPLE_OP_MODE=docked|handheld`, `NAPPLE_IPC_LENIENT=1` (error result instead of stopping, to enumerate gaps), `NAPPLE_HID_TRACE=1`.

**C8. Manual check, windowed, a few minutes per game** (on the branch build, then the same on the base build if anything looks wrong):
`runtime/build/napple-runtime "$D" --save-root /tmp/verify/saves-manual` (omit `--headless`). Link's Awakening: reach the title screen, start a new file and type a name
(the software keyboard path lives in `applet.c`, which was edited: the name must be accepted and shown correctly), walk around, check rumble and that the controller/keyboard respond,
check audio, open the map, save and reload. Super Mario Odyssey: reach the title screen, start, move and jump, check input, rumble, audio, and that nothing freezes. In both: any visible
difference from the base build (time of day, language, stuck waits at a network check, missing rumble, input stuck) is a finding.
Pass: behaves like the base. Fail: stalls, missing input/rumble/audio, wrong keyboard result, crashes.

**C9. Targeted spot checks that need no game** (run if the game run did not exercise them; each is fast):
 - `make -C runtime test-services` already covers the logic; additionally run `build/tests/test_svc_time` and read the `LoadTimeZoneRule` / snapshot lines to confirm the Mac's `/etc/localtime` gave a sensible device location (`readlink /etc/localtime`).
 - `NAPPLE_BATTERY=42,battery build/tests/test_svc_psm` is not meaningful (the test fixes its own state); instead rely on C7 for psm.

## 4. Regression guard (these must still pass because of what was touched)
- `make -C runtime test` (four pre-existing binaries) and `make -C runtime test-services`.
- LAS and SMO headless boots reach the same stage as the base, with the same or a smaller `missing_ipc` list (C6).
- Input still flows: scripted presses visible in `NAPPLE_INPUT_LOG` and moving the player in the manual check (hid.c edited).
- Rumble still works unless the game called `PermitVibration(false)` (hid.c edited).
- Software keyboard / controller-support applet still work in LAS (applet.c edited).
- Saves still persist (fs.c untouched, but confirm one save and reload in LAS).
- Time-dependent behaviour: the in-game clock/day-night or any date display is sane (time.c edited).
- `make -C input test`, `make -C audio test`, `make -C gfx test` are not affected by this branch; run them only if something in C6 looks off, to separate causes.

## 5. Fix policy
- **Build errors and clear typos:** fix directly on this branch, one commit per fix, message starting `verify:` (for example `verify: include <string.h> in bcat_core.c`). Re-run the affected check after each fix.
- **A failing test where the expectation came from the Linux mock** (for example a CHECK that depends on mock behaviour): fix the test, commit `verify: ...`, and say so in the report.
- **Needs a design decision** (a command id or layout that real titles contradict, a behaviour change that breaks a game and has two reasonable fixes, anything touching the kernel, `fs.c`, `nvdrv.c`, `gfx/`, `audio/`):
  do not guess. Stop that item, mark it `failed`, include the evidence, and carry on with the independent items.
- Do not edit `docs/runtime-services.md` or the author's notes except to append your report file `docs/changes/2026-10-24-services-VERIFY-REPORT.md` (commit message `verify: report`).
- **No push, no merge, no rebase of existing commits.** Leave the worktree `/tmp/napple-base` removed when done (`git worktree remove --force /tmp/napple-base`).

## 6. Report format
Finish with this table (and save it as `docs/changes/2026-10-24-services-VERIFY-REPORT.md`), one row per item, no prose substitutes:

| Item | Status (verified / fixed / failed / not run) | Evidence (command + the line that proves it) | Fix commit |
|---|---|---|---|

Rows to include: C1 build; C2 each of the four old tests; C3 each of the 25 new test binaries; C4 NPDM service list per game; C6 LAS boot, SMO boot; C7 one row per behaviour change 1-6
(exercised? what the log shows); C8 LAS manual, SMO manual; one row per new service (24 services: mii:u, caps:su, caps:u, psm, lbl, ncm, ns:am2, ns:ec, ns:vm, sfdnsres, spsm, bcat:u,
ldn:u, irs, audin:u, hidbus, pdm:qry, set:fd, olsc:u, ectx:aw, fgm, btm:u, ovln:snd) stating whether a game actually called it; rows for `time`, `applet`, `hid`, `offline` depth changes.
Definitions: **verified** = you ran it on this Mac and the evidence shows the expected result; **fixed** = it failed, you fixed it with a `verify:` commit and re-ran it; **failed** = still failing
(say why and whether it needs a decision); **not run** = you could not run it (say why).
End with two lists:
 1. *Safe to count as implemented now*: services/commands whose glue was exercised by a Mac unit test **and** (called by a game with correct behaviour **or** whose logic is fully
    covered by a passing Mac unit test with no layout doubt).
 2. *Must stay marked partial / unverified*: everything else, in particular any service no game called (it is only unit-tested) and any command whose id or layout is from memory and was not
    seen in a real request (compare the `in=` byte counts in the trace with the layouts in the source).
Also list every `command N not implemented` finding with service, command id and request size, and every behaviour change from section 2 that you saw affect a game.
