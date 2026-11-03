---
branch: claude/ipc-depth-sys-2026-11-01-r2
orchestration: ipc-depth-sys
base: 79415f0
head: 0666ec7 (code; the integrator file is one docs commit on top)
env: mac
date: 2026-11-01
areas: [runtime/services, runtime/tests]
status: integrated
---

# claude/ipc-depth-sys-2026-11-01-r2: IPC reference fixtures and missing command rows for the system / offline services

## Summary
- 69 fixtures `runtime/tests/ipc_reference/<interface>.txt` (format of the orchestration file; 748 application-reachable commands) for fsp-srv + file system objects, vi, time, acc, aoc, ns/ncm (`n`), pdm, pl, mii, olsc, bcat, caps, ldn, nifm, friend, prepo, nsd, bsd, sfdnsres, ssl, lm, pctl, ngc, ectx, ovln, mm, npns, notif, mnpp, ngct, nim:eca. Source: switchbrew command tables (ids, names, firmware), layout facts only.
- Every application-reachable command without a row now has one (about 335 rows): `runtime/src/services/sys_default.h` holds the documented safe defaults (success / zero output / never-firing event / the offline error the real service gives, e.g. no network for nifm and bcat, no network service account for friends). All of them are `IPC_STUB`, except the 28 pctl rows 1006..1603 which are the documented answers of an unrestricted console and are `IPC_IMPL`.
- Not done: `ns:am2`/`ns:vm`/`ncm`/`lr`/`miiimg`/`caps:c`-style system-only interfaces got fixtures with `app_reachable=n` and no new rows (an application cannot reach them); `ipc.c` and the fixture runner are ipc-depth-hid's. `ns:ec`, `friend:*`/`nifm:a|s`/`bsd:s|a` siblings share the interface fixtures of their `:u` form.

## Files
`runtime/src/services/`: new `sys_default.h`; rows added in `account.c bcat.c caps.c fs.c ldn.c lm.c misc.c ngc.c offline.c olsc.c ovln.c pdm.c push.c sfdnsres.c time.c vi.c`. `runtime/tests/ipc_reference/*.txt` (69 new), `runtime/tests/test_svc_sysdef_offline.c` (new), `test_svc_time.c test_svc_pctl.c test_svc_bcat.c` (extended; the aoc-rows-are-all-IMPL check now expects the seven default rows as documented defaults), `docs/archive/changes/2026-11-01-ipc-depth-sys-2026-11-01-r2.md`. No shared file touched, `runtime/Makefile` untouched (the new test is picked up by the `test_svc_*` wildcard).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| runtime build | verified | `make -C runtime -j4` clean (the only warning, unused `not_found` in `bcat.c`, predates this branch) |
| `make -C runtime test` | verified | all suites 0 failed |
| `make -C runtime test-services` | verified | every `test_svc_*` 0 failed (61 files) |
| fixtures vs tables | verified | a local script (see "How to test" 3) parses the fixtures and the `IpcCmd` tables: every `y` row has a table row, no table has a duplicate id |
| four-title boots (SMO, MK8D, ACNH, LAS, 60 s headless each, `tools/boot_test.sh` under the run lock) | verified | `runtime-boot.json`: `missing_ipc []` in all four, `stubbed_ipc` only the pre-existing `nvdrv:13` (SMO, ACNH, LAS); none of the new rows was hit, so no new stub answered a real title |

## Unbuilt / untested list
Nothing unbuilt. The error / zero-output defaults are unit-tested for nifm, ssl, friends, time, pctl, bcat only; the other ~250 rows are table entries over the same eight handlers in `sys_default.h` (no title exercised any of them).

## Findings
- The tables were short in the expected places: fsp-srv had 31 of ~70 application-reachable ids, nifm IGeneralService 10 of 54, IFriendService 11 of the user groups, ISslConnection 15 of 38, pdm:qry 8 of 21.
- `mii.c` builds its `IDatabaseService` tables with the `READ_CMDS` macro; the user variant (`mii:u`) is deliberately the read subset, so the fixture marks only those ids `y`.
- `push.c` answered ids 0..31 only (`npns:u` real ids start at 101, `notif:a` at 500): extended with ten and six rows.
- Decision (unattended): objects and buffers the console cannot back return an error result, not a fake object; the unknown-command sf result (`IPC_R_UNKNOWN_COMMAND`, what `NAPPLE_IPC_LENIENT` already gives) is used when there is no better module result. Network-dependent calls use the results the offline services already use.
- Limits of the fixtures: `app_reachable` for the big mixed interfaces (fsp-srv, IFriendService, IParentalControlService, ITimeZoneService) is a judgement from the wiki and libnx usage, written down as allow lists in the change note; correct if a title proves otherwise. Wiki names for a few rows are generic (`Unknown`, `S2`): left `n`.
- Outside my area, not touched: `runtime/src/ipc.c` (runner); `docs/architecture/runtime-services.md` rows (append-only, left to the integrator: the table row for fsp-srv still lists "stubs: ..." generically).

## How to integrate
- No ordering constraint. Conflict hotspots: the service files above (ipc-depth-hid owns hid / audio / etc., disjoint), `runtime/tests/test_svc_pctl.c` / `test_svc_bcat.c` if another branch edits them. Fixtures are new files only; the runner of ipc-depth-hid reads `runtime/tests/ipc_reference/*.txt`, so check it accepts the `# interface` header lines and the `n` rows.
- Build: `make -C runtime -j4`, `make -C runtime test`, `make -C runtime test-services` (needs the gfx static lib; copy `gfx/libnapple_gfx.a` and `gfx/build/obj/adapter/gfx_napple.o` from a built checkout if `make -C gfx` is too heavy).

## How to test
1. `make -C runtime test test-services` all green.
2. Boots: `lockf -k /Users/neto/NappleWork/.run.lock tools/boot_test.sh <titleid> 60` for 0100000000010000, 0100152000022000, 01006F8002326000, 01006BB00C6F0000, then `work/compat/<tid>/<build>/runtime-boot.json`: `counters.missing_ipc` must be `[]`.
3. Fixture check (not committed, ipc-depth-hid owns the runner): the generator and checker scripts used here (`parse.py extract.py gen.py cls.py gen2.py`) are in the shared notes folder `side-session-notes/local_67fc7cb9-.../ipc-depth-sys-tools/`.
4. `NAPPLE_IPC_LENIENT` unset: a title calling one of the new rows now gets the documented default instead of `runtime_fail`.

## Fix policy hints
Safe to fix directly: result-code choices (`sysd_err_*`), zero-output sizes in `sys_default.h` once a title shows the real size, fixture `app_reachable` flags. Needs a decision: replacing a stub by real behaviour (each row's name is in the table; start with the fsp-srv `OpenFileSystemWithId` family and `IGeneralService::EnumerateNetworkProfiles`).

## Needs from other areas
- ipc-depth-hid: the runner must skip `n` rows and treat `IPC_STUB` rows as present.
- `docs/architecture/runtime-services.md`: refresh the fsp-srv / nifm / friend rows to mention the documented defaults (append only).

## Integration result
- Date: 2026-11-01. Merged into main as 8cf2ce9 (integ/ipc-depth-sys 1953832, branch head 8c0cda2), after ipc-depth-hid (the runner). Outcome: **Small fix**.
- After the merge, `test_ipc_reference` read all 99 fixtures (30 hid + 69 sys) but reported 50 app-reachable ids missing, all binding problems of the static check, not missing rows:
  - `IDatabaseService` (16): mii.c builds its tables with the `READ_CMDS` macro, which the runner did not expand.
  - `npns:u` (21), `notif:a` (6), `nim:eca` (3), `mnpp:app` (2), `ngct:u` (2): push.c fills its tables at registration (generous range 0..31 + `extra()` calls), invisible to a source scan ("no command table is bound").
- `integrate:` fixes (2):
  1. 34abd60 `runtime/tests/test_ipc_reference.c`: an identifier between rows that names an object-like `#define` of the same file is expanded in place (depth-limited). Test code only.
  2. 1953832 `runtime/src/services/push.c`: the switchbrew-named rows (the former `extra()` rows plus the fixture ids inside the range) are static `IpcCmd` tables laid over the generous range at registration; every id keeps its previous handler and IPC_IMPL/IPC_STUB status (in-range rows only gain their names). The five fixtures got a `# table services/push.c:<name>_rows` line. `test_svc_push` (14 checks) unchanged and green.
- Result: `{"test":"ipc_reference","fixtures":99,"app_reachable":1116,"missing":0,"errors":0}`. No row was genuinely missing.
- Native ports: none (pure IPC tables; left as C on purpose).
- Tests: `make -C runtime test` + `test-services`: rc 0, 65 suites, all `"failed":0` (incl. `svc_sysdef_offline`, `svc_push`, `svc_pctl`, `svc_bcat`, `svc_time`).
- Four-title boots (runtime binary, 60 s headless, private copied `--save-root`, under `.run.lock`): SMO, MK8D, LAS, ACNH all rc 0, `notify-running`, `missing_ipc []`, no crashes, `stubbed_ipc` only `nvdrv:13`.
- GPL check: fixtures hold ids / names / firmware versions; `sys_default.h` and the rows are original code. No libnx code copied.
- Corrections to this file's instructions: "How to test" 2 uses `tools/boot_test.sh` (writes the shared `work/saves`); the integrator ran the runtime binary directly with a private save root instead. Step 3 (local scripts) is superseded by `make -C runtime test-ipc-reference`.
- Left for later: the `docs/architecture/runtime-services.md` refresh of the fsp-srv / nifm / friend rows (Needs from other areas) was not done here.
