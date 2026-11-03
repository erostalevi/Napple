# ipc-depth-hid (2026-11-01, r2): IPC reference lists, the runner, and the missing hid / audio / system-device rows

Branch `claude/ipc-depth-hid-2026-11-01-r2`, Mac. A command with no row stops the runtime (`runtime/src/ipc.c` `unimplemented()`); this round turns
"how deep is the IPC surface" into a measurement and fills the gaps of this package's services.

## What changed
- **Reference lists + runner.** `runtime/tests/ipc_reference/<interface>.txt` (30 files) list each interface's commands as
  `id<TAB>name<TAB>min_fw<TAB>y|n` with `# interface <name> (<source>)` first; `<name>` is the runtime's interface string, matched exactly.
  `runtime/tests/test_ipc_reference.c` reads the command tables from `runtime/src` (`IpcCmd x[] = {...}`), binds them to interface names
  (`IPC_OBJECT("name", table, ...)`, `SUB(fn, "name", table)`, `f(table, n, "name")`; files whose `service_register_*` is never called are skipped:
  `audin.c`), and fails when an application-reachable id has no row. `# alias` / `# table` directives cover bindings it cannot read. It runs in
  `make test` and `make test-host`; `make test-ipc-reference` prints details (rows the reference does not list, name differences).
- **hid**: 86 new rows, 148 of 148 app-reachable ids answered, all `IPC_IMPL` (the existing test asserts no hid row is a stub). Rows 133-136 were
  one id low (133 answered the center clamp; 136 ClearNpadCaptureButtonAssignment was missing): fixed, Clear takes only the aruid and clears every
  style. New state lives in `hid_core` (accelerometer parameters, passthrough, ERM commands, permit sessions, seven six-axis, Palma devices).
- **audio / audrec**: IAudioDevice 13 (two meanings, told apart by the output buffer), 14-21; IAudioRenderer 12/13; IAudioOut 12/13 (volume kept,
  not applied: `IPC_STUB`); IAudioIn 7/10; audin:u 5; hwopus 8/9; IHardwareOpusDecoder 8/9; IFinalOutputRecorder 10/11 (`audrec_flush`).
  The OpenAudioIn comment now matches the library: the microphone when permitted, silence otherwise.
- **settings**: `set` 11 is GetDeviceNickName (was answered as a key code map), 12 GetKeyCodeMapByPort; `set:sys` 53 time zone name (was at 62),
  62 GetDebugModeFlag (the old row answered 'U' there, i.e. debug mode on), 77, 79.
- **psm** 18; **lbl** 26 renamed EnableVrMode, 27-29; **spsm** named after switchbrew + 4/5; **btm** IBtmUserCore 19-21 renumbered, 1, 2, 22-37 added.
- `fgm`'s IRequest object is now `fgm::IRequest` (nifm's IRequest has the same name; the runner matches names exactly).

## Status
- *tested*: `make -C runtime test` (includes `test_ipc_reference`: 30 fixtures, 368 app-reachable ids, 0 missing), `make -C runtime test-services`
  (60 service tests, all pass; new checks in test_svc_hid, test_svc_hidcore, test_svc_audio, test_svc_audrec, test_svc_settings, test_svc_lbl,
  test_svc_spsm, test_svc_psm, test_svc_btm), `make test-host` target for the runner builds and passes on the Mac with `cc`.
- *verified*: four-title headless boots with this runtime (SMO, MK8D, LAS, ACNH, 60 s each, private save root): all reach `notify-running`, `missing_ipc`
  empty, no crashes, stub list unchanged (`nvdrv:13`), the same as the round-1 summaries. hid rows the titles call: 0, 1, 11, 21, 31, 66, 100, 102, 103,
  106, 109, 120, 121, 124, 128, 200, 203, 206, 1004; none of them calls a renumbered row (hid 133-136, set 11, set:sys 53/62, btm 19-21).
- Not verified on a title: none of the four titles calls the new hid / audio / btm rows (they were missing, and the titles boot), so the new rows are
  exercised by unit tests only. Layouts marked `[unverified]` in the sources (mode vibration, Palma data blocks, seven six-axis init, 1004/1005 on 20.0.0+).

## Files
`runtime/tests/test_ipc_reference.c`, `runtime/tests/ipc_reference/*.txt`, `runtime/Makefile` (appended), `runtime/src/services/{hid.c,hid_core.c,hid_core.h,
audio.c,audrec.c,audrec_core.c,audrec_core.h,settings.c,psm.c,lbl.c,lbl_core.h,spsm.c,btm.c,fgm.c}`, the matching `runtime/tests/test_svc_*.c`,
`docs/architecture/runtime-services.md` (appended).
