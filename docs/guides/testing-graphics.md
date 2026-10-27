# Testing graphics: golden frames and the gfx warning gate

## Goal
Make "no graphics bugs on LAS, SMO, ACNH and MK8D" a check, not an impression: scripted routes per title capture frames at fixed points, the frames are compared with
approved golden images, and the run fails on any gfx warning, Metal API / shader validation message, crash or frame that differs.

## What is where
| Piece | Path |
|---|---|
| Routes, frame numbers, tolerances, warning patterns | `tools/golden/routes.json` (committed) |
| The runner, diff, contact sheet, approve | `tools/golden/golden.py` (committed); `tools/golden/selftest.py` checks it without a game |
| Golden images | `~/Library/Application Support/Napple/goldens/<title>/<route>/frame-<N>.png` (outside the repository: they are game screenshots); `--golden-root` or `NAPPLE_GOLDEN_ROOT` moves them |
| Run output | `/private/tmp/napple-golden-runs/<stamp>/<title>/<route>/{frame-N.png,log.txt,result.json}`, `report.json`, `contact.html` |

## Steps
1. Build the runtime and the data once (data uses the small in-place mode, about 250 MB a title; needs the `napple` CLI, `cargo build --release -p napple-cli`):
   ```bash
   make -C input && make -C audio && make -C frontend && make -C gfx lib adapter && make -C runtime
   python3 tools/golden/golden.py build all
   ```
2. Run the routes (headless; Metal API validation is on, `MTL_DEBUG_LAYER=1`; `--no-validation` turns it off, `--shader-validation` adds `MTL_SHADER_VALIDATION=1`, which needs `NAPPLE_GFX_NO_ARCHIVE=1` (the harness sets it) and makes the GPU watchdog fire on heavy passes: LAS shows 2 `GPU Hang` errors in 110 s, no validation message):
   ```bash
   python3 tools/golden/golden.py run all            # or: run smo --route title-and-kingdom
   ```
   The exit status is 0 only when every route passed. Use an otherwise idle Mac: frame numbers depend on speed, and a loaded machine reaches fewer frames in the same time
   (a missing frame fails the route).
3. Open `contact.html` of the run (one section per route, one thumbnail per frame; green = within tolerance of the golden, orange = no golden yet, red = differs or never reached,
   plus the gfx warnings and validation messages found in the log). When the frames are right, approve them:
   ```bash
   python3 tools/golden/golden.py approve /private/tmp/napple-golden-runs/<stamp> [--title smo --route title-and-kingdom]
   ```
4. After a change to gfx, run the gate again: a frame that moved shows red with its mean and block difference.

## What counts as a failure
- the process exit status is not 0, or `CRASH` appears in the log;
- a log line matches `warning_patterns` of `routes.json`: `stale or invalid command handle`, `storage out of range`, `unsupported`, `unimplemented`, `orphan render target`,
  `EndRecording without BeginRecording`, `shader ... failed`, `could not be translated`, `is not inside a memory pool`, `not a mapped address`, view-compatibility warnings;
- a Metal validation message (`MTLDebug`, `failed assertion`, `validation failure`, `Shader validation`, a command buffer aborted, `GPU Hang`);
- a frame differs from its golden: mean absolute channel difference above `mean` (default 3, scale 0-255) or an 8x6-cell block of the 64x36 downsample above `max_block` (default 40).
A translator problem found by shader validation belongs to the shader translator (`gfx/src/shader`): report it, do not patch around it in the replay.

## Routes
The first routes are rough (input times are guesses until a person has watched them): LAS boot to the village, SMO title and first kingdom (frames 8000 and 9000 are the sleep
scene that showed exploded geometry), ACNH airport counter, MK8D title to a race. Refine a route by editing `routes.json` (inputs are `NAPPLE_INPUT_SCRIPT`, see
[environment variables](../reference/environment-variables.md)); add routes for beach/forest (LAS), name entry and island arrival (ACNH), cup select (MK8D) the same way.

## Pitfalls
- A golden is only as good as the person who approved it: look at the contact sheet before `approve`.
- Frames depend on the scripted timing; if the machine is slower the same frame number shows an earlier scene. Compare runs on the same load.
- API validation costs little; shader validation makes runs several times slower and is off by default.
- Goldens must never be committed.
