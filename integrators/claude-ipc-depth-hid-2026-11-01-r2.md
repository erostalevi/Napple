---
branch: claude/ipc-depth-hid-2026-11-01-r2
orchestration: ipc-depth-hid
base: 79415f0
head: 525c48c
env: mac
date: 2026-11-01
areas: [runtime/services, runtime/tests, runtime/Makefile, docs]
status: ready
---

# claude/ipc-depth-hid-2026-11-01-r2: IPC reference lists + runner, and the missing rows for hid, audio and the system-device services

## Summary
- New fixture format and runner: `runtime/tests/ipc_reference/<interface>.txt` (`# interface <name> (<source>)`, then `id<TAB>name<TAB>min_fw<TAB>y|n`)
  and `runtime/tests/test_ipc_reference.c`, which reads the command tables from `runtime/src` (no running runtime needed), binds them to interface
  names and fails when an app-reachable id has no row. 30 fixtures for this package (hid + sub-interfaces, hidbus, irs, audren/audout/audin/audrec/hwopus
  + sub-interfaces, psm, apm, fgm, lbl, spsm, set, set:sys (partial), set:fd + iterator, btm:u + IBtmUserCore). 368 app-reachable ids, 0 missing.
- hid: 86 new rows (148/148 app-reachable). Fixed rows 133-136 (one id low; 136 was missing). Audio: IAudioDevice 13-21, IAudioRenderer 12/13,
  IAudioOut 12/13 (stub: volume not applied), IAudioIn 7/10, audin:u 5, hwopus 8/9, IHardwareOpusDecoder 8/9, IFinalOutputRecorder 10/11.
- settings: set 11 is GetDeviceNickName (was a key code map), 12 added; set:sys time zone 62 → 53, 62 = GetDebugModeFlag (old row made it read as
  "debug mode on"), 77, 79. psm 18; lbl 26-29 (26 renamed); spsm renamed to switchbrew + 4/5; btm IBtmUserCore 19-21 renumbered, 1, 2, 22-37 added.
- `audio.c` OpenAudioIn comment refreshed (microphone when permitted, silence otherwise).
- Not done: set:fd rows 10/11/20-27 (dev-mode only, marked `n`); psm 17 and unnamed rows (marked `n`). No hidbus/irs/apm/fgm rows were missing.

## Files
`git diff --stat 79415f0..HEAD`: 56 files. New: `runtime/tests/test_ipc_reference.c`, `runtime/tests/ipc_reference/*.txt` (30),
`docs/archive/changes/2026-11-01-claude-ipc-depth-hid-2026-11-01-r2.md`, this file. Changed (owned): `runtime/src/services/{hid.c,hid_core.c,hid_core.h,
audio.c,audrec.c,audrec_core.c,audrec_core.h,settings.c,psm.c,lbl.c,lbl_core.h,spsm.c,btm.c,fgm.c}`, `runtime/tests/test_svc_{hid,hidcore,audio,audrec,
settings,lbl,spsm,psm,btm}.c`. Shared, append-only: `runtime/Makefile` (test_ipc_reference rules, `test`/`test-host` prerequisites, `test-ipc-reference`),
`docs/architecture/runtime-services.md` (new section at the end). `runtime/src/ipc.c` was not changed.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: reference lists + runner over every fixture in the directory | tested | `make -C runtime test` → `{"test":"ipc_reference","fixtures":30,"app_reachable":368,"missing":0,"errors":0}`; also `make -C runtime build/host/test_ipc_reference` (HOST_CC) passes |
| Acceptance 2: hid 71, 73-78, 136, 207-210, 212, 303-306 and all other app-reachable missing ids | tested | hid fixture 148/148; new checks in `test_svc_hid` (331 checks, 0 failed) and `test_svc_hidcore` (53, 0 failed) |
| Acceptance 3: OpenAudioIn comment | done | `runtime/src/services/audio.c` IAudioIn section comment and `ain_open_at` |
| Acceptance 4: `make -C runtime test` and `test-services` green | tested | both exit 0; 60 service tests, all `"failed":0` |
| Acceptance 4: four-title boots, no new refusals | verified | runtime binary directly, `--headless --seconds 60`, private `--save-root $TMPDIR/ipc-depth-hid/saves`, under `.run.lock`: SMO, MK8D, LAS, ACNH all `last_stage notify-running`, `missing_ipc []`, 0 crashes, stubs unchanged (`nvdrv:13`), same as round-1 summaries |
| New rows on a real title | not verified | none of the four titles calls a new or renumbered row (calls seen: hid 0,1,11,21,31,66,100,102,103,106,109,120,121,124,128,200,203,206,1004) |

## Unbuilt / untested list (the integrator checks these first)
None unbuilt: everything compiled and the unit tests ran on this Mac. Untested on a title: all added rows (unit tests only).

## Findings
- **Misnumbered rows fixed** (switchbrew/libnx ids): hid 133-135 (were SetNpadAnalogStickUseCenterClamp/SetNpadCaptureButtonAssignment/Clear at
  133/134/135; now 134/135/136, 133 = SingleWithDestination); set 11 (GetKeyCodeMap3 → GetDeviceNickName); set:sys 62 (time zone → GetDebugModeFlag, time zone
  at 53); lbl 26 (name only); spsm 2/3 (names only, same answers); btm IBtmUserCore 19-21. The tests that encoded the old ids were corrected
  (`test_svc_hid` 133-135, `test_svc_settings` key-map ids {7,9,11} → {7,9,12}, `test_svc_btm` 19-21): these are id corrections, not weakened checks.
- Decision: fixtures name interfaces by the runtime's exact interface string (no suffix matching), so ipc-depth-sys must use e.g. `IRequest` for nifm's
  object; fgm's object was renamed `fgm::IRequest` to stop sharing it. `# alias` / `# table` directives exist for bindings the parser cannot read.
- Decision: app_reachable `y` = reachable by an application on firmware ≤ 22.x (removed commands stay `y`); `n` = system/debug-only services (lbl, spsm,
  fgm, set:sys, set:fd), Switch 2 rows, 23.0.0+, unnamed rows, hid 86-88 calibration and 3000-3015 debug maps.
- Decision: all new hid rows are `IPC_IMPL` (`test_svc_hid` asserts no hid row is a stub); several keep state the host cannot act on (seven six-axis
  writes no samples, gesture ranges stored, Palma operations complete instantly). Layouts marked `[unverified]` in code.
- `audin.c` is dead code (services.c does not register it; audio.c owns audin:u). The runner skips it. Its IAudioIn table uses different ids.
- Runner sees one table with no interface name: `services/mii_img.c:root_cmds` (marked `(void)root_cmds`, unused).
- Name-only differences remain (libnx vs switchbrew names, e.g. StartAudioOut vs Start, RunDpdProcessor vs RunPointingProcessor); `make test-ipc-reference` lists them.

## How to integrate
- No ordering constraint. ipc-depth-sys (parallel) writes fixtures in the same format into `runtime/tests/ipc_reference/`; after both land, run
  `make -C runtime test-ipc-reference` — a fixture whose interface name matches no runtime string shows "no command table is bound" and fails.
- Conflict hotspots: `runtime/Makefile` (append-only block at the end), `docs/architecture/runtime-services.md` (section at the end). Other sessions
  should not touch the service files here.
- Build: `make -C runtime && make -C runtime test && make -C runtime test-services` (expected: all exit 0, ipc_reference missing 0).
- No Linux-only calls used. `test_ipc_reference.c` is portable C on purpose (runs in `test-host` on Linux sessions).

## How to test
1. `make -C runtime test` → ends with `{"test":"ipc_reference",...,"missing":0,"errors":0}`; failing shows `missing <id> <name>` lines under the interface.
2. `make -C runtime test-services` → every line `"failed":0` (test_svc_hid 331 checks, test_svc_btm 45, test_svc_settings 55, test_svc_audio 22, test_svc_audrec 68).
3. Boots: `lockf -k /Users/neto/NappleWork/.run.lock runtime/build/napple-runtime work/games/<tid>/<build> --headless --seconds 60 --summary /tmp/x.json --save-root <private>`
   for 0100000000010000, 0100152000022000, 01006BB00C6F0000, 01006F8002326000 (ACNH build ff1d1c05670db602): `missing_ipc` empty, `notify-running`.
4. Human: none needed; Palma (Poke Ball Plus) and GC rumble would need a title that uses them.

## Fix policy hints
- Safe to fix directly: fixture typos, a name/id in a fixture, includes.
- Needs a decision: if a title shows one of the renumbered rows behaving differently than expected (hid 133-136, set 11, set:sys 53/62, btm 19-21), check the
  title's call against switchbrew before reverting; the old ids came from memory, the new ones from switchbrew + libnx.

## Needs from other areas
- audio/ (libnapple_audio): a per-stream gain so IAudioOut SetAudioOutVolume (12) can be applied (row is `IPC_STUB` until then).
- input/: none required; Palma connection handles rely on the backend reporting the Palma style for a slot (it already does when the title announces it).
