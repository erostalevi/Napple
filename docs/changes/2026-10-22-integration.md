# Integration: runtime + gfx + audio + input + frontend (2026-10-22)

## Objective
Link every component into one `napple-runtime` and reach real rendered frames in Link's Awakening (LAS) and Super Mario Odyssey (SMO).

## Files
runtime: `app.m` (replaces frontend.m: frontend library window, headless mode), `movie.c/.h`, `input_script.c`, `services/{pl,audio,nvdrv,offline}.c`, `main.c` (game.json, NVN version detection, NAPPLE_WAIT_SECS), `gfx_glue.c` (screenshots, movie tick), `signals.c` (NAPPLE_WATCH/PEEK), `threads.c` (backtraces in thread dumps), `services/applet.c` (message event de-assert), `memory.c` (SetMemoryAttribute uncached), `services/fs.c` (trace), Makefile.
gfx: adapter (lazy override table, screenshot, texture API), `core.c`, `api_texture.c` (host texture write), `api_cmd.c` (command-handle graveyard), `mtl_replay.m` (window-origin fix, copy by draw), `mtl_core.m` (headless pacing). audio: audren REV<5 output size. crates/napple-build: `--dump-fonts`. tools/prepare_game.sh. docs.

## Logic and decisions
- Root causes found: adapter override table built after the runtime consumed it (nvdrv "missing"); message event never de-asserted (guest spun at 100%); zero-count `GetReleasedAudioOutBuffer` returned stale stack (crash); command handles used after buffer finalize; SMO's NVN version check (53.13) silently skipped graphics init; Y flip emulation was wrong (see gfx-interface.md section 10).
- LAS movies are VP9 WebM played through the SDK movie library; NVDEC is not emulated, the host decodes with libvpx (movie.c).
- nv::InitializeGraphics stays native with a bookkeeping nvdrv.

## Flow
guest -> nn::init::Start -> services (HIPC) -> nvnBootstrapLoader (gfx) -> NVN record -> replay thread -> Metal -> present (pacing) ; movie decoder -> libvpx -> NVN texture.

## Risks / pending (ordered)
1. Window mode (frontend lib) is built but untested: macOS asked the owner for Documents access for napple-runtime; not clicked.
2. SMO: text glyphs missing, `nvnMemoryPoolMapVirtual`, H.264 movies, further services (mii:u, ns, ...).
3. Frame rate ~40-45 fps in both games (replay thread is mostly idle; guest-side/CPU swizzle of the movie texture); speed correctness vs game logic not yet measured.
4. Audio only verified with the null sink; HID Npad offsets, controllers, saves unverified; kernel timeslicing not implemented; gmem (SSBO compute) not configured (compute dispatches skipped).
5. Packaged apps embed an older runtime: rebuild them to get the NVN auto-detection and fixes.

## Tests
`make -C gfx test` and `make -C audio test` pass. Headless runs with screenshots: work/shots/las-4000.png (title), las-7200.png (scene), smo-400.png, smo-3000.png.
