# perf-las-live (2026-10-25): LAS slowdowns measured in live play

Interactive session: the user played LAS windowed (default settings, default save root) while `NAPPLE_PERF_LOG=1` and `tools/long_monitor.sh` logged every 5 s.

## Findings
- **The sustained ~30 FPS state did not reproduce** in 5 live runs (3-6 min each, sword on the beach -> village -> forest north). Memory plateaued (it does not leak without bound). It may need a cold shader/pipeline cache (new build or translator version) or a longer session.
- **Stutters = first-use pipeline/shader builds on the draw thread.** `mtl_pipe_get` builds a new pipeline synchronously on first use (deliberate: nothing skipped). Every stalled 5 s window in runs 2-4 had 7-29 new pipelines and 3-28 new shaders; stalls 170-535 ms. With the blocking-build counter (run 4): windows with 400, 778 and 1262 ms of blocking builds, max single build 333 ms. Run 3 (first pass over the route) had 10 of 44 windows stalled; run 4 (caches warm) 3 of 74; run 5 (warm) 0 of 49. The cost is paid once per area; the binary archive/shader cache makes the second visit smooth.
- **Command-list graveyard grew up to ~2 GB.** `grave_add` (gfx/src/api_cmd.c) released at most 256 idle/finalized recordings per second, but live play creates more (heap: 290k plain mallocs, 2 GB in Malloc Small; `nvnCommandBufferEndRecording` is the top malloc site in MallocStackLogging). The backlog grew toward `GRAVE_CAP` (262144 x ~7.5 KB). Fix: no per-pass cap. First 230 s peak RSS: runs 2-4 2.6 / 2.9 / 3.4 GB, run 5 (fix) 1.96 GB. Not an isolated A/B (route differs slightly); the counter `cmdrecs live` in the perf line now makes it measurable (run 5 max 2408 live recordings).
- Other footprint at ~3.7 GB peak: guest memory ("Untagged") ~1.2 GB, IOAccelerator ~320 MB, Malloc Small 1.9 GB (mostly the above).
- Audio latency drifted 21 -> 44 ms over 4 min (output stats line); no underruns. Not investigated.
- Game FPS sometimes reads 85-125 for a window right after a stall: the game catches up (not a timescale bug).

## Changes (all env-guarded or bounded)
- `gfx/src/api_cmd.c`: graveyard releases everything idle/dead each pass (dynamic list); `gfx_cmdrec_live()`.
- `gfx/src/mtl_present.m`: `NAPPLE_PERF_LOG=1` prints a `[perf]` line every 5 s (fps, worst frame gap, GPU ms, pipelines/shaders built, blocking build ms, live recordings).
- `gfx/src/mtl_shader.m`, `internal.h`, `obj.h`: blocking pipeline build time counters.
- `tools/long_monitor.sh` (RSS/threads/CPU sampler), `tools/long_run_perf.sh` (headless run + summary).

## Verified
`make -C gfx test` and `make -C runtime test` pass; LAS headless 45 s exit 0 (0 stalled windows); live runs above. SMO not run: its data is not on this Mac.

## Not done / next
- Remove first-visit stutter: build pipelines off the frame path (async with fallback, or a shipped warm archive). Needs a design decision (skipped draws vs. blocking).
- Guest memory (~1.2 GB) and GPU texture memory for iOS.

## SMO live run (same build, user played ~10 min from the title menu into the first kingdom)
- 51 windows, 4 stalled (>150 ms; worst 990 ms, at the scene change into gameplay: 656 pipelines built in one window), otherwise a steady 60 fps, GPU 6-12 ms. Peak RSS 2.0 GB.
- `cmdrecs live` peaks at ~112k (SMO builds thousands of display lists per second) and drains after the 30 s idle window: no unbounded growth. Headless 45 s boot: exit 0.
- Visual bugs seen (not from this branch; the user says they predate it): shattered cap brim and white ribbons on the title menu, exploded geometry and a half-scale duplicate view in the top-left ~53% of the frame in-game. Hand-over note in the session notes folder (smo-title-screen-visual-bugs.md).

## SMO glitches (user-driven follow-up, same branch)
- **Half-scale duplicate view in the top-left ~53% (FIXED, user-confirmed live).** With render scale on, SMO's G-buffer passes mixed attachments: colour 0 is a 2D-array texture that `mtl_scale_pick_size` never scales (plain 2D only) next to scaled siblings and depth. Metal renders the overlap only (1600/3024 = 0.53 of the frame) and the viewport followed colour 0 (scale 1). `pass_unify_scale` (mtl_replay.m) + `mtl_tex_force_scale` (mtl_tex.m) lift unscaled same-size attachments of a pass to the largest factor. Found with `NAPPLE_SCALE_DIAG=1` (logs passes whose attachments disagree). Still open: one 400x228 pass (probably a mipmapped target, skipped on purpose).
- **Register-indexed constant-buffer reads past the bound size now return 0** (hardware behaviour) instead of reading the neighbouring ring memory: `cbr(cb, size, off)` with the sizes table at `[[buffer(30)]]` (`CBSZ_BUFFER_INDEX`), TRANSLATOR_VERSION 7. Found because one skinned draw binds a 0xc0-byte palette and indexes bone 11. Not proven to affect the visible glitches.
- **Exploded geometry (NOT fixed).** Reproduced headlessly and deterministically enough: `NAPPLE_INPUT_SCRIPT="30:A:0.3,45:A:0.3,60:A:0.3"` boots SMO into the first kingdom (frames ~7000-9000 show it, also with `NAPPLE_RSCALE=0`, so independent of render scale). Bisecting with `NAPPLE_DEBUG_SKIP_VS` (one vertex program per frame, three frames each, screenshot in the middle) shows the large wedge comes from the skinned program `fa3fa31651fa2f19` (3 draws in frame 7996; bone-index stream attr 11 is R8_UINT stride 1 = repacked per draw; all bone indices 11; palette in UBO 3, 12 bones x 0x30). Ruled out: render scale, the stride-1 stream repack row range (forcing the full range changes nothing), out-of-range palette reads. Not yet checked: the translation of the program's maths (compare against a CPU evaluation of one vertex), the RGB10A2 normal / RGBA8 snorm decode, the 0x30-byte matrix layout. Other glitch families the user sent (grass blade streaks, purple sky, dithered dark Mario silhouette, shattered cap brim on the title menu) are probably separate programs; the bisect recipe above finds each.
