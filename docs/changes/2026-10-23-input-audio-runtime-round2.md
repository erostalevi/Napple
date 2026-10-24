# Input, audio, saves, services, frame pacing: round 2 (2026-10-23)

## Objective
Advance Link's Awakening (LAS, `01006BB00C6F0000`) and Super Mario Odyssey (SMO, `0100000000010000`) on the shared fronts: full input with
remapping, audio verification, transactional saves, services, frame pacing and the settings overlay, and verify all of it from the packaged
`.app` launched the way a double-click does. Tree health was checked first (`make -C gfx test`, `make -C gfx lib adapter`, `make -C runtime`,
`make -C packaging`, `cargo build --release`, `cargo test --workspace`: all green, no half-edited files found; `services/applet.c` is complete).

## Files
runtime: `src/services/hid.c` (vibration IPC, config dir), `src/services/fs.c` (transactional save data), `src/services/services.c` (operation mode),
`src/services/vi.c` (drift-free vsync), `src/services/audio.c` (stats, thread-safe init), `src/app.m` (settings push to gfx, USR1 dump, audio warm-up off the main thread,
hot keys), `src/main.c` (gfx ready hook), `src/gfx.h` + `src/gfx_stub.c` (`gfx_set_settings`), `src/input_script.c` (atomic chords), `Makefile` (`make test`),
`tests/` (`test_ipc.c`, `test_fs_journal.c`, `test_hid_service.c`, `stubs.c`, `check.h`).
input: `include/napple_input.h`, `src/{input.c,bindings.c,gamepad.m,internal.h}`, `tests/{test_input.c,test_bindings.c}`.
frontend: `src/overlay.m` (Controls tab rewritten, frame-rate list), `src/frontend.m` (hot keys, Esc cancels capture, key-window log, test aids), `include/napple_frontend.h`.
gfx (tiny additive hook, as agreed): `adapter/gfx_napple.c` gained `gfx_set_settings` (maps `GfxSettings` onto `napple_gfx_set_settings`; nothing else touched).
tools: `synth_keys.m` (real CGEvents), `refresh_app_runtime.sh` (swap the runtime into built apps and re-sign). Docs: `runtime-services.md`, `audio-input-frontend-interface.md`.

## Logic and decisions
**Input.**
- HID layout validated against both games with `NAPPLE_HID_TRACE=1` (every guest read of the shared memory, with the reading instruction): LAS and SMO read only the Npad
  entry of player 1 (offset 0x9A00): style word at +0, then the full-key lifo ring header at +0x30 (tail/count/latest) and 0x30-byte entries (sampling number, buttons
  u64, two sticks, attributes), plus the full-key six-axis ring. Offsets and entry size match `napple_hid_layout` (LATEST), including the SDK's reads of the sampling
  numbers; no unhandled access forms. (The layout of the other style rings is still from public documentation: no game read them.)
- Bindings gained: outer range, response curve, trigger press point, per-axis invert, npad style preference, console mode (see interface doc 2.2b). The style preference only applies
  if the game's `SetSupportedNpadStyleSet` allows it. Diagonal keyboard sticks are normalised to the unit circle (they read 1.41 before).
- Chords (L+R) are pressed in one lock (`napple_input_keys`): the scripted input used two calls and the game occasionally saw L and R in different 120 Hz samples.
- Rumble: `SendVibrationValue(s)` were no-op stubs, so rumble never reached CoreHaptics. They now decode the handle ({type, style, npad id, device}), clamp, remember (for
  `GetActualVibrationValue`) and drive `ng_rumble`. Not verified on hardware (no controller available in this session).
- Home / Guide and Share / Capture on a controller call a hot-key callback (overlay / screenshot); F1 / F12 on the keyboard.
- `NAPPLE_INPUT_LOG=1` logs player 1's merged state changes. `--save-root` now also moves `input.json` and `settings.json` (they ignored it before).
- Operation mode (docked / handheld) comes from input.json (`operation_mode`) or `NAPPLE_OP_MODE`; applet, apm and vi already read `g_cfg.op_mode_handheld`, nothing set it.
**Frame pacing.** The overlay's Video settings did nothing: the runtime never passed them to gfx. `app.m` now pushes them (start and every change; headless runs read the same
settings.json). `fps_cap` 0 means the game's own pacing (the old "unlimited" label would have run fixed-step games at 4-5x speed), fixed rates are documented as fast
forward / slow motion, 1000 = uncapped. The vsync thread used `nanosleep(period)` per tick (drift = slower game); it now uses absolute deadlines and re-reads the period.
**Saves.** Transactional save data, see `runtime-services.md` "Save data". Decision: snapshot-by-clone of the whole directory (saves are KBs to MBs) instead of per-file journaling,
because the SDK commits whole file systems; the live directory is stable so open file descriptors survive a commit. Restore is built beside the live directory first (found when a
sandboxed test hit EPERM: the first version removed live before cloning).
**Audio.** Output stats (`audio: output: N frames requested, U underruns ...`) every 15 s and at exit. The engine start used to reset the master volume to 1.0 after the frontend had set the saved
one: the volume is re-applied. Engine start moved off the main thread (a cold Core Audio unit blocked the window for ~10 s once under LaunchServices; never reproduced: 0.2-0.4 s in 6 other launches).
**Debug aids.** `kill -USR1 <pid>` thread dump, `NAPPLE_FS_TRACE` (also saves), `NAPPLE_FE_OVERLAY`/`NAPPLE_FE_VIEWSHOT`, `tools/synth_keys.m`.

## Flow
keyboard / mouse / GameController -> `napple_input` (120 Hz sampler, bindings, curves) -> HID shared memory (host alias, mapped into the guest) -> guest reads rings;
overlay (F1 / Home) -> bindings edit -> input.json (atomic) ; settings -> `app.m push_gfx_settings` -> `gfx_set_settings` ; guest `fs` write -> live dir -> Commit -> clone to `.committed`.

## Default controls (keyboard)
| Switch | Key | | Switch | Key |
|---|---|---|---|---|
| Left stick | W A S D | | A / B / X / Y | L / K / O / I |
| Right stick | arrow keys | | L / R | Q / E |
| D-pad up/left/down/right | T F G H | | ZL / ZR | Left Shift / Space |
| Left / right stick click | Z / C | | + / - | Return / Tab |
| Home (opens the Napple menu) | F1 or Cmd+, | | Capture (screenshot) | F12 |
| Full screen | Cmd+Return or Ctrl+Cmd+F | | Quit | Cmd+Q |
Mouse: left button = touch screen (the pointer position inside the game view). Controllers (Xbox, PlayStation, MFi, Switch Pro / Joy-Con) are mapped automatically: face buttons by
position (south = B, east = A, west = Y, north = X; Nintendo pads by label), LB/RB = L/R, LT/RT = ZL/ZR (press point adjustable), sticks, d-pad, Menu = +, View/Options = -, Home/Guide = menu,
Share = screenshot. Link's Awakening's title screen needs L+R together (Q+E), after the intro movie and the second short movie (~40 s).

## Verification (evidence)
- `make -C input test`, `make -C frontend test`, `make -C runtime test` (ipc 27 checks, fs_journal 16, hid_service 10), `make -C gfx test`, `cargo test --workspace`: pass.
- Real key events: `tools/synth_keys` posts CGEvents (HID tap) to the frontmost app; the LAS `.app` launched with `open` (`work/scratch/apptest.sh`): `window is key (first responder game view)`, title -> L+R -> new game
  cinematic (work/shots/ap3-3400.png, 1080p docked), 0 audio underruns in 80 s. In-process scripted input (`NAPPLE_INPUT_SCRIPT`) still works for both games.
- Saves, LAS: the owner's save (File 1 + Autosave "Marin and Tarin's House") adopted and listed on the file select screen after relaunch, Continue reaches the game (work/shots/l7-2400.png, l7-4000.png);
  the game commits (5 commits at start). SMO: `File1.bin` + `Common.bin` created and committed after Start. Unit tests cover rollback, interrupted commit, nested dirs, adoption.
- Audio (Core Audio output at 5% volume, 95 s LAS / 70 s SMO): 0 underruns, 0 overruns, 0 trims, latency 15-21 ms headless, 22-38 ms windowed; frames requested = wall time x 48 kHz (no drift). Not judged by ear.
- Frame rate setting: `fps_cap` 30 in settings.json (headless) gives ~27 presents/s with LAS's movie advancing at the slowed rate (fixed pacing works); default 0 keeps real time. In the packaged windowed LAS app the game ran ~59-60 presents/s (headless runs ~46).
- Overlay Controls tab rendered to PNG (work/shots/ov3.png).
- SMO from the title: Joy-Con notice -> title -> Start (Assist Mode screen, text now renders) -> opening demo (audio stream `StSyD001_DemoOpening.bfstm`, 14.8 MB read at its bitrate, picture black) ->
  `+` skips -> save files created -> Cascade Kingdom data loads -> "Look Around" HUD prompt, 3D picture black (gfx). No missing service/command (`NAPPLE_IPC_LENIENT` list empty). The H.264 tutorial movie
  (`PlayGuideMovieRStick.mp4`, decoder type 1) is left to the SDK library, whose NVDEC path fails quietly: the game continues (graceful skip).

## Risks and pending (ordered)
1. SMO 3D picture is black in the opening demo and in game (gfx: compute dispatches / skipped draws 7301); LAS in-game scene white-out (gfx).
2. Frame rate ~46 presents/s in LAS (GPU 11 ms + pacing 14-16 ms per present, not pipelined): fixed-step logic would run at ~77% speed. Delta-time driven parts (movies, audio) are real time. Owner: gfx replay performance.
3. Rumble, hot-plug, multiple pads, gyro and Share / Home hot keys are implemented but not exercised on real hardware here (no controller). Unit tests cover the mapping.
4. No time-slicing for busy-wait threads (documented in runtime-services.md); no title needs it yet.
5. H.264 (SMO tutorial movies) is skipped, not decoded (VideoToolbox plan: Annex-B csd-0/1 -> format description, per-frame AVCC samples, NV12 into the client Buffer).
6. `mii:u`, `ns`, `ncm`, `lbl`, `psm`, `caps:*` remain unimplemented (loud failure) because neither game asked for them yet.
7. Apps launched from `~/Documents/...` trigger the macOS Documents-folder prompt when `--save-root` points there; the default root (`~/Library/Application Support/Napple`) does not.
8. The overlay tab bar was replaced by a segmented control after the last app refresh; `work/apps` hold a runtime from just before that (run `tools/refresh_app_runtime.sh` when no app is running). SMO also logs a Metal shader error (`point_coord` undeclared) and presents 300+ fps on its first screens (gfx).
9. After editing the runtime, rebuild apps (`napple build`) or run `tools/refresh_app_runtime.sh <app>`; Studio copies whatever `runtime/build/napple-runtime` is at build time.

## Tests
`make -C input test`, `make -C frontend test`, `make -C runtime test`, `make -C gfx test`, `cargo test --workspace`. End to end: `work/scratch/{gpurun,wintest2,apptest,usr1test}.sh` (take `work/gpu.lock`).
