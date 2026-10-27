# Report a graphics bug

## Goal
Turn "it looks wrong" into evidence a graphics maintainer can act on: which scene, which frame, what the log says, and whether it changes with settings. Use it with the *Graphics bug* issue form.
Related: [debugging.md](debugging.md), [add-nvn-function.md](add-nvn-function.md), [add-maxwell-shader-op.md](add-maxwell-shader-op.md), [symptoms.md](../symptoms.md) (Rendering).

## Never include
Game files, keys, firmware, extracted textures, models or shader binaries ([legal rules](../../CONTRIBUTING.md#legal-rules-first)). A screenshot of your own screen showing the bug is fine. Do not upload more frames than needed to show it.

## 1. Check the log first
Run headless and read the graphics lines:
```bash
mkdir -p /tmp/napple-save
runtime/build/napple-runtime <data-dir> --headless --seconds 45 --save-root /tmp/napple-save 2> /tmp/napple-boot.log
grep -E 'napple-gfx\] (warn|error)|stats:' /tmp/napple-boot.log
```
`unimplemented NVN entry point called: <name>` and `shader ... could not be translated` are the usual causes of missing geometry; `stale or invalid command handle` points at command-buffer lifetime. Put those lines in the report. The final `stats: frames N ...` line shows whether frames flow at all.

## 2. Capture frames
`NAPPLE_SCREENSHOT=<prefix>` writes `<prefix>-<frame>.png` for chosen presented frames (at most 20 files); `NAPPLE_SCREENSHOT_FRAMES=300,600,900` picks them (default every 120th). Example:
```bash
NAPPLE_SCREENSHOT=/tmp/shot NAPPLE_SCREENSHOT_FRAMES=600,900 NAPPLE_INPUT_SCRIPT="10:A,14:A" \
  runtime/build/napple-runtime <data-dir> --headless --seconds 30 --save-root /tmp/napple-save 2> /tmp/napple-boot.log
```
Note the frame numbers of the good and the bad frame, and the input script that reaches the scene. A script that reaches the scene reproducibly makes the bug fixable.

## 3. Narrow it down (optional but valuable)
- `NAPPLE_DRAWLOG=600-601` logs every draw of those frames; `NAPPLE_DRAWLOG_QUIET=1` keeps it short; `NAPPLE_DRAWLOG_TEX=1` logs bound textures. `NAPPLE_DRAWLOG_RT=<dir>` writes the render targets of logged passes as images ([environment variables](../reference/environment-variables.md)).
- `NAPPLE_GFX_CMDTRACE=1` logs command-buffer submissions. `NAPPLE_PERF_LOG=1` prints fps and GPU time every 5 seconds (for stutter and slowness reports).
- Render targets and drawlogs can contain game imagery: attach one or two small crops, not full sets, and say you have the rest.
- Say whether it changes with the render scale or aspect setting once those exist, and whether it appears only after a long run.

## 4. Golden frames and the gfx tests
The graphics tests under `gfx/tests/` (`make -C gfx test`, macOS only) compare small synthetic scenes drawn through the NVN API with expected results; `make -C gfx tests` builds them without running. If a bug can be shown by a few NVN calls on synthetic data, a failing test in that style is the best report ([testing.md](testing.md)).
Golden-frame comparison for the four reference titles is being added by the graphics maintainers; when it lands its README under `tools/golden/` is the reference and you will be able to attach a frame diff.
<!-- TODO(gfx-correctness): link tools/golden/README.md here once it is on main. -->

## 5. Write the report
Game, title id, version; Napple build; Mac and display; the scene and how to reach it; frame numbers; the warnings from step 1; what it should look like; and whether it is every run. Open it with the *Graphics bug* form.
