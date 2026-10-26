# Environment variables

Every environment variable the code reads, collected from `getenv`, `std::env::var`, `std::env::var_os`, `ProcessInfo.processInfo.environment` and `os.environ` in
`runtime/`, `gfx/`, `audio/`, `input/`, `frontend/`, `crates/`, `studio/`, `packaging/` and `tools/`. Paths are relative to the `Napple/` project root.

Conventions used by the code, unless a row says otherwise:

- "set" means the variable exists in the environment (`getenv(...) != NULL`), whatever its value, including `0`. "on" means set and not `0`/empty, as the row states.
- Most `NAPPLE_*` variables for the runtime, gfx, audio and input are diagnostics for developers. They are read once, at first use, so they must be set before the process starts.
- Command-line options of the runtime are in [cli.md](cli.md#runtime-binary-napple-runtime); `-v` and `--trace FILE` set `NAPPLE_VERBOSE` and `NAPPLE_TRACE` for the process.
- Variables that the launcher or Studio only set (never read) are marked "set by".
- Debug aids whose values are lists (`NAPPLE_SHADER_PROBE`, `NAPPLE_DEBUG_SKIP_VS`) have their exact format in the comment next to the `getenv` call in the file named in the last column.

## Runtime: logging, tracing, diagnostics

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_VERBOSE` | unset = off; any non-empty value other than `0` = on | Verbose runtime log (`LOGV` lines, per-command IPC lines). Same as `-v`. | `runtime/src/log.c` |
| `NAPPLE_TRACE` | path; unset = no trace | Opens the file for writing and records JSON-lines trace events (svc, IPC, unsupported svc). Same as `--trace FILE`; the option sets this variable. | `runtime/src/log.c`, `runtime/src/main.c` |
| `NAPPLE_FILE_LOG` | unset (default), `0`, `all` | When stderr is neither a terminal nor a regular file (apps started from Finder or `open`), stderr is captured to `~/Library/Logs/Napple/<titleid>.log` (5 MB cap, 3 files). `0` disables the capture; `all` keeps every line instead of the filtered default. | `runtime/src/filelog.c` |
| `NAPPLE_SUMMARY` | path; unset = none | Where to write the compatibility stage file `runtime-boot.json`. Same as `--summary FILE`, which takes precedence. | `runtime/src/main.c`, `runtime/src/log.c` |
| `NAPPLE_EVIDENCE` | string; default empty | Copied into the `evidence` field of the stage file written for `NAPPLE_SUMMARY`. `tools/boot_test.sh` sets it to the log path. | `runtime/src/log.c`, `tools/boot_test.sh` |
| `NAPPLE_SVC_TRACE` | set = on | Logs every svc and its result. Same as `--svc-trace`. | `runtime/src/main.c` |
| `NAPPLE_HLE_TRACE` | `0`, `1`, `2` | Host-call tracing for HLE overrides: `1` counts calls and prints a summary, `2` also logs the first call of each. Any other value aborts with "hle trace mode must be 0, 1 or 2". | `runtime/src/main.c`, `runtime/src/hle_stack.c` |
| `NAPPLE_TRACE_IPC` | `all` | With `all`, logs every IPC command (service, command id, buffer counts) even without `-v`. Any other value has no effect in this code. | `runtime/src/ipc.c` |
| `NAPPLE_IPC_LENIENT` | set = on | An unimplemented IPC command returns an "unknown command" error to the guest, and an unimplemented `sm` service returns "not found", instead of aborting the run. Also lets a failing `ldr:ro` NRO load return an error instead of aborting. | `runtime/src/ipc.c`, `runtime/src/ro.c` |
| `NAPPLE_SVC_LENIENT` | set = on | An unimplemented svc returns `R_NOT_IMPLEMENTED` instead of aborting, to list several unsupported svcs in one run. | `runtime/src/kernel.c` |
| `NAPPLE_PCPROF` | set = on | Starts a sampler thread that logs the program counters of guest threads (`pcprof` lines) to find where a hung boot spins. | `runtime/src/threads.c` |
| `NAPPLE_PCPROF_REGS` | set = on (needs `NAPPLE_PCPROF`) | Also logs the registers of the thread with id `0x51` about every 300th sample, for the first 6000 samples. | `runtime/src/threads.c` |
| `NAPPLE_PEEK` | `<module>+<hex offset>`, for example `main+0x24741f8` | Logs the 8 bytes at that module offset at the points where the runtime calls `runtime_peek`: the `nvnBootstrapLoader` request (`runtime/src/gfx_stub.c`) and the crash report. | `runtime/src/signals.c` |
| `NAPPLE_WATCH` | `<module>+<hex offset>` | Makes that page read-only and logs every guest store to the 8-byte location (pc, caller, value); the faulting store is decoded and emulated. | `runtime/src/signals.c` |
| `NAPPLE_WAIT_SECS` | integer seconds | Prints the pid and sleeps that long before the guest starts, to attach a debugger. | `runtime/src/main.c` |
| `NAPPLE_FS_TRACE` | set = on; value starting with `a` = all reads | Logs RomFS reads of 0x10000 bytes or more (every read when the value starts with `a`). `tools/romfs_lookup.py` maps such log lines to RomFS files. | `runtime/src/services/fs.c` |
| `NAPPLE_HID_TRACE` | set = on | Protects the guest mapping of the HID shared memory (re-armed every 20 ms) and logs each distinct offset the guest reads (offset, size, value, caller). | `runtime/src/services/hid.c` |
| `NAPPLE_HID_TRACE_STACK` | set = on (needs `NAPPLE_HID_TRACE`) | Adds guest stack return addresses to each HID trace line. | `runtime/src/services/hid.c` |
| `NAPPLE_APPLET_TRACE` | set = on | Turns on guest-side call tracing (`ipc_trace_guest(10)`) when the guest creates a library applet. | `runtime/src/services/applet.c` |
| `NAPPLE_TIME_LOG` | seconds as a number (`time` service log: any set value) | In `timescale.c`, a positive number starts a thread that logs guest seconds per real second, presents per second and vsync ticks per second at that period. In `runtime/src/services/time.c`, any set value logs the first five `ToCalendarTime` calls. | `runtime/src/timescale.c`, `runtime/src/services/time.c` |

## Runtime: kernel and memory

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_ALIAS_GIB` | integer 1..64; default 64 | Size in GiB of the guest alias region to reserve. When set, the reservation does not shrink below this value; when unset it shrinks in 4 GiB steps down to the minimum. Values outside 1..64 abort. | `runtime/src/memory.c` |
| `NAPPLE_FREE_CORES` | set = on | Disables the emulated per-core scheduling (core affinity masks): threads run on any core. | `runtime/src/threads.c` |
| `NAPPLE_SKIP_VERIFY` | set = on | Skips the SHA-256 check of module images against `manifest.json`. | `runtime/src/module.c` |
| `HOME` | path | Base of the default save root `~/Library/Application Support/Napple` (`--save-root` overrides) and of the log directory `~/Library/Logs/Napple`. Falls back to `.` (or `/tmp` in the input library). | `runtime/src/main.c`, `runtime/src/filelog.c`, `input/src/input.c`, `frontend/src/settings.c`, `packaging/launcher.c` |
| `NAPPLE_VERSION` | version directory name | Selects which embedded version directory under `Contents/Resources/game/` the launcher (and on iOS the runtime) uses; otherwise the first line of `game/default`. A name containing `/` or starting with `.` is rejected. Also read by the build scripts below. | `packaging/launcher.c`, `runtime/src/ios_platform.m` |
| `NAPPLE_BUNDLE_DIR` | set by the launcher | The launcher exports `<App>.app/Contents` before starting the runtime. Nothing in the runtime reads it. | `packaging/launcher.c` |

## Services

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_OP_MODE` | `docked` or `handheld` | Console operation mode seen by `appletOE`, `apm` and `vi`. `handheld` selects 720p; any other non-empty value selects docked (1080p). Overrides `operation_mode` in `input.json`; fixed for the life of the process. | `runtime/src/services/services.c` |
| `NAPPLE_LANG` | Switch language code (at most 7 characters) | Language reported to the guest. A longer value aborts. Takes precedence over `NAPPLE_LOCALE`. | `runtime/src/services/host_info.c` |
| `NAPPLE_LOCALE` | BCP 47 tag such as `pt-BR` | Language and region reported to the guest when `NAPPLE_LANG` is not set. Default: the Mac's current locale. | `runtime/src/services/host_info.c` |
| `NAPPLE_BATTERY` | `NN` or `NN,ac` or `NN,battery`; default 100 percent, on AC when docked | Battery percentage (clamped to 0..100) and charger state reported by `psm`. A value that does not start with a number is ignored. | `runtime/src/services/psm.c`, `runtime/src/services/psm_core.c` |
| `NAPPLE_PLAYER_SELECT` | profile index, or `cancel`; default 0 | Answer of the player-select library applet: the profile at that index, or cancel. | `runtime/src/services/applet.c` |
| `NAPPLE_SWKBD_ANSWER` | text, `<cancel>` or `<initial>` | Answers the software keyboard without UI (tests and automation): the given text, cancel, or the request's initial text. Headless runs without this variable accept the initial text. | `runtime/src/app.m`, `frontend/src/frontend.m`, `frontend/src/ios/frontend_ios.m` |
| `NAPPLE_SWKBD_UTF16` | set = on | Returns the software keyboard text as UTF-16 instead of UTF-8 in the applet output storage, for titles that need it. | `runtime/src/services/applet.c` |
| `NAPPLE_CONTROLLER_PADS` | integer 1..8 | Number of controllers reported as connected; values above 8 become 8. Default: the number of GameControllers found, at least 1. | `runtime/src/app.m` |
| `NAPPLE_MOVIE_HOST` | `0` | `0` does not register the host `movie::Decoder` overrides, so the game's own movie library runs. Any other value keeps them. | `runtime/src/movie.c` |
| `NAPPLE_MOVIE_H264_TYPE` | integer; default 1 | `DecoderType` value that the host decoder treats as H.264 (video/avc). Diagnostics. | `runtime/src/movie.c` |
| `NAPPLE_MOVIE_PACE` | `0` or other; default off | Non-zero paces decoded frames by presentation time; `0` (and the default) hands every decoded frame over at once. | `runtime/src/movie.c` |
| `NAPPLE_MOVIE_NEGATIVE` | `0`, `1`, `2`; default 0 | Negative controls for the probe movie only: `1` swaps U and V, `2` suppresses the format-changed event. | `runtime/src/movie.c` |

## Speed, input script, screenshots (runtime glue)

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_SPEED` | number greater than 0, for example `2.5`; default: the setting from the frontend | Game speed multiplier; overrides the percentage chosen in the overlay. | `runtime/src/app.m` |
| `NAPPLE_SPEED_MODE` | `0`, `1`, `2`; default: frontend setting | Which clocks follow the speed: `0` game clock and vsync, `1` vsync only (clocks stay real), `2` game clock only. | `runtime/src/app.m`, `runtime/src/timescale.h` |
| `NAPPLE_INPUT_SCRIPT` | `"<sec>:<BUTTON>[+<BUTTON>][:<hold>],..."`, for example `5.0:A,12:PLUS:0.3` | Scripted input for automated runs: presses the named button or stick direction at that many seconds after the guest first configures HID, held `<hold>` seconds (default 0.15). Up to 4096 events. Button names are listed at the top of the source file. | `runtime/src/input_script.c` |
| `NAPPLE_SCREENSHOT` | path prefix; unset = off | Writes `<prefix>-<frame>.png` for chosen presented frames (at most 20 files). | `runtime/src/gfx_glue.c` |
| `NAPPLE_SCREENSHOT_FRAMES` | comma-separated frame numbers (up to 64); default every 120th frame | Frames to capture when `NAPPLE_SCREENSHOT` is set. | `runtime/src/gfx_glue.c` |
| `NAPPLE_FG` | integer; non-zero = on | Overrides the frame generation setting. | `runtime/src/app.m` |
| `NAPPLE_FG_MULT` | integer; 0 = automatic, 2..8 | Overrides the frame generation multiplier. | `runtime/src/app.m` |
| `NAPPLE_FG_METHOD` | integer 0..4: auto, flow, flow low latency, Metal flow, blend | Overrides the frame generation method (`NAPPLE_FG_*` in `gfx/include/napple_display.h`). | `runtime/src/app.m` |
| `NAPPLE_SUPERSAMPLE` | number, 1 or more | Overrides the supersampling factor. | `runtime/src/app.m` |
| `NAPPLE_RENDER_MODE` | integer 0..3: native, scale, window, fixed | Overrides the render size mode. | `runtime/src/app.m` |
| `NAPPLE_RENDER_SCALE` | number; 1 = native | Overrides the render scale multiplier (used by render mode `scale`). | `runtime/src/app.m` |
| `NAPPLE_FILTER` | integer 0..5: bilinear, nearest, box, bicubic, Lanczos, sharp | Overrides the output filter. | `runtime/src/app.m` |
| `NAPPLE_UPSCALER` | integer; `0` none, `1` MetalFX spatial | Overrides the upscaler. | `runtime/src/app.m` |

## AArch32 interpreter and recompiler

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_A32_JIT` | `0` | `0` ignores `a32jit.dylib` and interprets everything. Any other value or unset uses the dylib when it is present and matches the modules. | `runtime/src/a32jit/a32jit.c` |
| `NAPPLE_A32_JITSTATS` | `1` or `2` | Any set value prints dispatcher statistics at exit; `2` also profiles the exits to the dispatcher and prints the profile. | `runtime/src/a32jit/a32jit.c` |
| `NAPPLE_A32_JITLOG` | file path | Appends to that file the guest addresses reached through computed jumps without a lifted block. The output can be fed back as `A32LIFT_SEEDS`. | `runtime/src/a32jit/a32jit.c` |
| `NAPPLE_A32_RECOMPILE` | `1`; unset or `0` = off | During `napple build`, lifts the AArch32 modules of each version and installs `a32jit.dylib` and `a32jit.manifest` next to them (same as `napple rewrite a32`). | `crates/napple-build/src/tables32.rs` |
| `A32LIFT_ONLY` | comma-separated module names | Debugging for the lifter tool: lifts only the named modules; the rest are interpreted. | `runtime/src/a32jit/a32lift_main.c` |
| `A32LIFT_SEEDS` | file path | Lifter tool: extra entry points, one hex guest address per line (bit 0 = Thumb). Default: `a32jit.seeds` in the modules directory when it exists. | `runtime/src/a32jit/a32lift_main.c` |
| `NAPPLE_A32_SVCTRACE` | set = on | Logs each AArch32 svc with its arguments and result. | `runtime/src/a32/a32_proc.c` |
| `NAPPLE_A32_TRACE` | set = on | Records executed program counters in a 4096-entry ring and prints the last 40 in the crash report; execution then uses a different run loop than the normal one. | `runtime/src/a32/a32_proc.c` |
| `NAPPLE_A32_CALLS` | set = on | Allocates a 1024-entry ring per CPU that records call targets (`bl`, `blx`); the last 60 are printed in the crash report. | `runtime/src/a32/a32_proc.c` |
| `NAPPLE_A32_WATCH` | set = on | Logs where a guest thread is (pc, lr, sp, r0-r3) every 256 million instructions it keeps running. | `runtime/src/a32/a32_proc.c` |
| `NAPPLE_A32_DUMP` | set = on | When a call runs for 128 slices, dumps 24 bytes at each of r0-r3 that look like guest pointers. | `runtime/src/a32/a32_proc.c` |
| `NAPPLE_A32_NVNTRACE` | set = on | Logs each NVN call made from AArch32 code with r0-r3 and lr. | `runtime/src/a32/a32_hle.c` |
| `A32_WATCH_PC`, `A32_WATCH_ADDR`, `A32_WATCH_OFF` | hex numbers | Watch aid compiled only when `A32_WATCH` is defined at build time: at entry to `A32_WATCH_PC` it watches the word at `r0 + A32_WATCH_OFF`, or watches `A32_WATCH_ADDR` directly, and logs changes. | `runtime/src/a32/a32_exec.c` |

## Graphics (gfx)

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_GFX_LOG` | integer 0..3 (error, warn, info, debug); default 2 | Log level of the gfx library. | `gfx/src/core.c` |
| `NAPPLE_GFX_PROF` | set = on | Accumulates timing counters (for example sync wait time). | `gfx/src/core.c`, `gfx/src/api_device.c` |
| `NAPPLE_ORIGIN_FLIP` | `0` | `0` restores the older lower-left origin convention (debug). Default on. | `gfx/src/core.c` |
| `NAPPLE_TURBO` | number | Switches pacing to fixed and sets the target frame rate to this value, or 300 when the value is 1 or less. For headless benchmarks. | `gfx/adapter/gfx_napple.c` |
| `NAPPLE_GFX_SKIPINIT` | set = on | Skips `napple_gfx_init` in the adapter. | `gfx/adapter/gfx_napple.c` |
| `NAPPLE_NV_INIT_NOOP` | set = on | Adds a no-op override for `nv::InitializeGraphics`. | `gfx/adapter/gfx_napple.c` |
| `NAPPLE_RSCALE` | `0` | `0` disables gfx-level render target scaling (the per-game escape hatch also in the display settings). | `gfx/src/mtl_scale.m` |
| `NAPPLE_MINIMAL_LAYOUT` | `0` | In the texture layout code, textures with flag `0x200` and no packaged block height use the minimal-layout rule; `0` turns that rule off (a negative control for fresh sessions). Default on. | `gfx/src/tex/napple_layout.c` |
| `NAPPLE_REC_BUDGET_MB` | integer MB greater than 0; default 1024 | Memory budget for finalized command list recordings before the least recently used are released. | `gfx/src/api_cmd.c` |
| `NAPPLE_GFX_MATH` | `safe` (default), `relaxed`, `fast` | Metal shader math mode. `safe` disables fast math; `relaxed` and `fast` use `MTLMathMode` on macOS 15 / iOS 18 or later (`fast` also enables fast math). | `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_NO_ARCHIVE` | set = on | Does not open the Metal binary archive `pipelines-v<N>.metalar` in the cache directory. | `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_CACHE_MAX_MB` | integer MB, `0` = unlimited; default 1024 | Size cap of the shader cache directory (translated shaders and pipelines archive); the oldest files are removed first when it is exceeded. | `gfx/src/cache_trim.c`, `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_NO_PREWARM` | set = on | Does not rebuild persisted pipelines in the background when their programs are registered. | `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_PIPE_SEED` | colon-separated file paths | Extra pipeline-key manifests (same format as `pipelines.bin`) merged into the prewarm list, in addition to `pipelines-seed.bin` in the cache directory. | `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_PIPE_SKIP` | set = on | Diagnostic: pipelines are built asynchronously and `mtl_pipe_get` does not wait for or build a queued pipeline itself. | `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_SPECULATE` | non-zero integer = on; default off | Opt-in speculative pipeline building. | `gfx/src/mtl_shader.m` |
| `NAPPLE_GFX_SHADER_LOG` | `0` | `0` disables the periodic shader build progress lines (every 2 seconds). Default on. | `gfx/src/mtl_shader.m` |
| `NAPPLE_SHADER_DUMP` | directory | Writes the Maxwell code of any program that fails to translate to `<dir>/<hash>_s<stage>.bin`. | `gfx/src/mtl_shader.m` |
| `NAPPLE_SHADER_DUMP_HASH` | hex program hash | With `NAPPLE_SHADER_DUMP`, also dumps that program even when it translates. | `gfx/src/mtl_shader.m` |
| `NAPPLE_SHADER_PROBE` | `<fs hash hex>:<pc hex>:<rA>:<rB>:<rC>[:<scale>]`, several joined by `;` | Debug aid: the fragment program outputs those registers as its colour; several probes are cycled. | `gfx/src/mtl_shader.m`, `gfx/src/mtl_replay.m` |
| `NAPPLE_SHADER_PROBE_PERIOD` | integer frames; default 30 | How often the active probe changes. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DEBUG_SOLID_FS` | fragment program hash (hex) | Draws that program's geometry in solid magenta. | `gfx/src/mtl_shader.m` |
| `NAPPLE_SAVE_MSL` | set = on | Annotates translated MSL with the Maxwell pc and instruction word of each instruction. | `gfx/src/shader/maxwell_translate.c` |
| `NAPPLE_DEBUG_SKIP_VS` | `<first frame>:<hash>,<hash>,...` (hashes in hex) | From that relative frame, frame `first+i` skips the draws of the i-th vertex program, to bisect which program draws a wrong shape. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DEBUG_SKIP_DRAWN` | `<first frame>:<first draw>` | Frame `first+i` skips the draw with ordinal `first draw + i`. | `gfx/src/mtl_replay.m` |
| `NAPPLE_NO_ORPHAN` | set = on | Disables the stopgap that presents the composite render target when the window draws sample nothing large. | `gfx/src/mtl_replay.m` |
| `NAPPLE_ORPHANLOG` | set = on | Logs the choice made by the orphan present stopgap (first 10). | `gfx/src/mtl_replay.m` |
| `NAPPLE_SHOT_COMPOSITE` | set = on | Screenshots and presents the debug composite texture instead of the window texture. | `gfx/src/mtl_present.m` |
| `NAPPLE_SCALE_DIAG` | set = on | Logs every pass whose attachments do not all match guest size times scale. | `gfx/src/mtl_replay.m` |
| `NAPPLE_RCLOG` | set = on | Logs draws skipped because render enable was off (every 1000th). | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG` | `N` or `N-M` (relative frame numbers) | Per-draw diagnostics on stderr for those frames. A live capture request (SIGUSR2 in the runtime) sets the range itself. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_QUIET` | set = on | Keeps `NAPPLE_DRAWLOG` active but not verbose. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_UBO` | integer; default 24 | Float words of each bound uniform buffer printed per draw. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_UBOHEX` | set = on | Also prints uniform buffer contents as hex. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_VBFULL` | set = on | Prints 4096 instead of 24 words of each vertex buffer. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_TEX` | set = on | Logs the textures bound by logged draws (once per texture). | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_RT` | directory | Writes the render targets of each logged pass there as `pass<N>-...ppm`. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_RT_ALL` | set = on | With `NAPPLE_DRAWLOG_RT`, dumps every colour attachment (`pass<N>-rt<i>-...`) instead of the first. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_EACH` | set = on | With `NAPPLE_DRAWLOG_RT`, splits the pass after every draw so each draw's result is dumped. | `gfx/src/mtl_replay.m` |
| `NAPPLE_DRAWLOG_GRID` | set = on | Prints exact values at a 3x3 grid of pixels instead of writing images. | `gfx/src/mtl_replay.m` |
| `NAPPLE_CAPTURE_DIR` | directory; default current directory | Where live capture screenshots `cap<serial>-<frame>.png` are written. | `gfx/src/mtl_replay.m` |
| `NAPPLE_GFX_EVENT_FS` | fragment program hash (hex) | Event-relative frame counting: frames are counted from the first draw that uses that program. | `gfx/src/mtl_replay.m` |
| `NAPPLE_GFX_EVENT_SHOTS` | comma-separated relative frame numbers (up to 64) | Frames, relative to the event, to screenshot. | `gfx/src/mtl_replay.m` |
| `NAPPLE_GFX_EVENT_PREFIX` | path prefix; default `event` | Prefix of the event screenshots `<prefix>-<frame>.png`. | `gfx/src/mtl_replay.m` |
| `NAPPLE_OPLOG_FRAME` | frame number | Logs every recorded op replayed in that frame. | `gfx/src/mtl_replay.m` |
| `NAPPLE_GFX_CMDTRACE` | set = on | Logs command buffer submissions and recording life cycle events. | `gfx/src/api_device.c`, `gfx/src/api_cmd.c` |
| `NAPPLE_NVNTRACE` | set = on | Logs every recorded NVN op in record order. | `gfx/src/api_cmd.c` |
| `NAPPLE_PRESENTLOG` | integer N | Logs the window textures at each present once N presents have happened (first 12 lines). | `gfx/src/api_window.c` |
| `NAPPLE_TEXLOG` | set = on | Logs the first 20 textures created with the display flag. | `gfx/src/api_texture.c` |
| `NAPPLE_REGTRACE` | set = on | Logs the first 40 texture pool registrations of textures 1280 pixels wide or more. | `gfx/src/api_texture.c` |
| `NAPPLE_REGSLOT` | integer pool slot | Logs every registration into that texture pool slot. | `gfx/src/api_texture.c` |
| `NAPPLE_MAPLOG` | set = on | Logs virtual to physical memory pool mappings. | `gfx/src/api_memory.c` |
| `NAPPLE_PERF_LOG` | set = on | One statistics line every 5 seconds (fps, worst frame gap, GPU and output ms, pipelines built). Used by `tools/long_run_perf.sh`. | `gfx/src/mtl_present.m` |
| `NAPPLE_GFX_DIRTY_DIAG` | non-empty, not starting with `0` | Diagnostics for guest write tracking (dirty page tracking). | `gfx/src/dirty.c` |
| `NAPPLE_FG_HEADLESS` | set = on | Allows frame generation to run without a window layer (headless tests). | `gfx/src/mtl_interp.m` |

## Audio

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_AUDIO_OFF` | set = on | Forces the null sink even when a device was requested. | `audio/src/engine.c` |
| `NAPPLE_AUDIO_DEVICE` | integer; non-zero = on | Headless runs use the null sink by default; a non-zero value opens the Core Audio device anyway. Windowed runs always use the device. | `runtime/src/services/audio.c` |
| `NAPPLE_AUDIO_VOLUME` | float; default 1.0 (or the frontend setting) | Master volume. When set it takes precedence over the volume chosen in the frontend settings. | `runtime/src/services/audio.c`, `runtime/src/app.m` |
| `NAPPLE_AUDIO_PREFILL_MS` | integer 0..500; default 20 | Output prefill in milliseconds (never below 5 after clamping). Values outside the range are ignored with a log line. | `audio/src/engine.c` |
| `NAPPLE_AUDIO_STATS` | non-empty, not starting with `0` | Each audio renderer logs render statistics about every 15 seconds. | `audio/src/audren.c` |
| `NAPPLE_TEST_DEVICE` | set = on | Test only: `audio/tests/test_engine.c` also opens the real device for a smoke test. | `audio/tests/test_engine.c` |

## Input

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_INPUT_NO_GC` | set = on | Does not start GameController discovery and ignores controller snapshots. | `input/src/input.c` |
| `NAPPLE_INPUT_LOG` | set = on | Logs virtual pad button changes (and iOS touch hits). | `input/src/input.c`, `frontend/src/ios/frontend_ios.m` |
| `NAPPLE_HID_DUMP` | file path | Writes the HID shared memory to that file when input stops. | `input/src/input.c` |
| `NAPPLE_HID_STYLE` | integer, decimal or `0x` hex | Forces the controller style bits reported to the guest (a `NAPPLE_STYLE_*` mask). `NAPPLE_STYLE_HANDHELD` alone makes the pad handheld only. | `input/src/input.c` |

## Frontend

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_FE_OVERLAY` | integer tab index | Test aid: opens the overlay on that tab at start. | `frontend/src/frontend.m` |
| `NAPPLE_FE_VIEWSHOT` | `<png path>:<seconds>` | Test aid: renders the overlay view to a PNG after that many seconds (the Metal layer is not captured). | `frontend/src/frontend.m` |
| `NAPPLE_DEMO_OVERLAY` | set = on | `frontend/tests/demo_window.m` opens the overlay after 2 seconds. | `frontend/tests/demo_window.m` |

## Build, CLI, packaging and Studio

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `NAPPLE_TMPDIR` | directory; default the system temporary directory | Where `napple build` stages the bundle and keeps the RomFS cache. Used only when it is on the same volume as the output directory; otherwise the output directory is used. | `crates/napple-build/src/util.rs` |
| `NAPPLE_REA_BIN` | path to the `rea` executable | The REA (Ghidra) command that `napple sigs resolve` runs. Default `<work>/tools/node_modules/.bin/rea`. | `crates/napple-sigs/src/reverser.rs` |
| `GHIDRA_INSTALL_DIR` | path | Passed to REA. Default `/opt/homebrew/opt/ghidra/libexec` when that exists. | `crates/napple-sigs/src/reverser.rs` |
| `JAVA_HOME` | path | Passed to REA. Default the Homebrew `openjdk@21` home when that exists. | `crates/napple-sigs/src/reverser.rs` |
| `GH_CONFIG_DIR` | path; default `$HOME/.config/gh` | GitHub CLI configuration directory used by `napple sigs share`. | `crates/napple-sigs/src/share.rs` |
| `PATH` | list of directories | Used to find `git`, `gh` and other tools; passed on to child processes. | `crates/napple-sigs/src/exec.rs`, `crates/napple-sigs/src/repo.rs`, `crates/napple-sigs/src/share.rs`, `crates/napple-sigs/src/reverser.rs` |
| `NAPPLE_CLI` | path to the `napple` executable | Studio uses it first when looking for the CLI (then the bundled copy, the installed engine, `target/release/napple`, `/usr/local/bin`, `/opt/homebrew/bin`). Studio's first-launch setup writes it to `<support>/env.sh`. | `studio/Sources/NappleStudioCore/NappleCLI.swift`, `studio/Sources/NappleStudioCore/EngineInstaller.swift` |
| `NAPPLE_HOME` | directory | Set by Studio's first-launch setup in `<support>/env.sh` (the Application Support Napple directory). Nothing in this repository reads it. | `studio/Sources/NappleStudioCore/EngineInstaller.swift` |
| `NAPPLE_VERSION` | version string, default `0.1.0` | Version number written to the Studio app and release scripts (not the same use as in the launcher row above). | `studio/build_app.sh`, `studio/packaging/make_release.sh`, `tools/ci_build.sh` |
| `NAPPLE_BUILD` | build number, default `1` | Bundle build number for Studio builds. | `studio/build_app.sh`, `tools/ci_build.sh` |
| `NAPPLE_REBUILD` | non-empty | `studio/build_app.sh` rebuilds the CLI, runtime and launcher even when binaries exist. | `studio/build_app.sh` |
| `NAPPLE_STUDIO_SCRATCH` | directory; default `${TMPDIR:-/tmp}/napple-studio-build` | Swift scratch directory for the Studio build (kept outside the repository tree). | `studio/build_app.sh` |
| `NAPPLE_SIGN_IDENTITY` | codesign identity | Re-signs Studio with the hardened runtime and signs the DMG. Without it the app is ad-hoc signed. | `studio/packaging/make_release.sh`, `tools/ci_build.sh` |
| `NAPPLE_NOTARY_PROFILE` | `notarytool` keychain profile | With `NAPPLE_SIGN_IDENTITY`, notarizes and staples the app and DMG. | `studio/packaging/make_release.sh` |
| `NAPPLE_SKIP_TESTS` | non-empty | `tools/ci_build.sh` skips its test run. | `tools/ci_build.sh` |
| `SDK` | `iphoneos` (default) or `iphonesimulator` | Selects the iOS SDK for the iOS build scripts. | `tools/build_ios.sh`, `tools/build_ios_deps.sh` |
| `IOS_SDK` | SDK path | Overrides the sysroot found with `xcrun`. | `tools/build_ios.sh`, `tools/build_ios_deps.sh` |
| `DEPS` | directory | Prefix with libopus (and libvpx) for the iOS runtime. Default `build-ios-deps/prefix` (or `prefix-sim`). | `tools/build_ios.sh` |
| `OPUS_SRC`, `VPX_SRC` | source directories | Use your own libopus and libvpx checkouts instead of downloading. | `tools/build_ios_deps.sh` |
| `VPX` | `1` | Also builds libvpx for iOS. Default `0`. | `tools/build_ios_deps.sh` |
| `CC`, `OPUS`, `VPX`, `GFX`, `HOST_CC`, `OPT`, `CFLAGS`, `OBJCFLAGS`, `EXTRA_LIBS` | make variables (`?=`) | Overridable defaults in the Makefiles (compiler, library paths such as `/opt/homebrew/opt/opus`, the gfx back end, optimization flags). They are make variables, not read with `getenv`, but make also takes them from the environment. | `runtime/Makefile`, `audio/Makefile`, `gfx/Makefile`, `input/Makefile`, `frontend/Makefile`, `packaging/Makefile` |

## Developer scripts and tests

| Variable | Values / default | Effect | Defined in |
|---|---|---|---|
| `REWRITER` | `rust` (default) or `python` | `tools/prepare_game.sh` uses the Rust rewriter, or the reference Python stopgap. | `tools/prepare_game.sh` |
| `SAVE_ROOT` | directory | `tools/legacy/flaky_boot.sh` passes it as `--save-root`; default the runtime's default save root. | `tools/legacy/flaky_boot.sh` |
| `SCRIPT` | input script string | `tools/dev/dev_apptest.sh` passes it as `NAPPLE_INPUT_SCRIPT`. | `tools/dev/dev_apptest.sh` |
| `SHOT_FRAMES` | comma-separated frames; default `2400,3000,3600` | `tools/dev/dev_apptest.sh` passes it as `NAPPLE_SCREENSHOT_FRAMES`. | `tools/dev/dev_apptest.sh` |
| `OVERLAY_TAB` | integer | `tools/dev/dev_wintest2.sh` passes it as `NAPPLE_FE_OVERLAY`. | `tools/dev/dev_wintest2.sh` |
| `DM_RULESET` | default `native` | Ruleset of the reference Python rewriter. | `tools/refpy/patch_nso.py` |
| `DM_PRIVATE_GATES` | `0` disables; default on | Private gates option of the reference Python rewriter. | `tools/refpy/patch_nso.py` |
| `FUZZ_DEBUG` | set = on | Prints the first few mismatching samples in the AArch32 fuzzer. | `runtime/tests/a32/fuzz.py` |
| `HOST_TEST_LOG` | set = on | Enables log output in the host test stubs. | `runtime/tests/host/host_stubs.c` |
| `TMPDIR` | directory; default `/tmp` | Used for temporary files by `tools/build_a32jit.sh`, `studio/build_app.sh`, `tools/linux-check/run-tests.sh` and one runtime test. | `tools/build_a32jit.sh`, `studio/build_app.sh`, `tools/linux-check/run-tests.sh`, `runtime/tests/test_svc_fs_dataid.c` |
| `NAPPLE_FIXTURE_DIR` | directory | Required by the ignored Rust test `dump_fixture_for_golden`, which writes the table fixture for regenerating the golden file. | `crates/napple-build/src/tables.rs` |
| `NAPPLE_SHADER_CORPUS` | directory | `gfx/tests/t_xlate.m` also translates every shader file in that directory. | `gfx/tests/t_xlate.m` |
| `T_STRICT`, `T_DUMP`, `T_COUNT`, `T_DIRTY_SECS`, `T_ARCH_PASS`, `T_CACHE_PASS`, `T_PK_PASS`, `T_PK_NOSPEC`, `T_STRESS_PASS`, `T_STRESS_THREADS`, `T_STRESS_NO_SHADER_PREWARM`, `T_STRESS_NOFLOOD` | test-specific | Options of the gfx test programs; their meaning is in the test source. | `gfx/tests/nvn_harness.h`, `gfx/tests/t_nvnapi.c`, `gfx/tests/t_dirty.c`, `gfx/tests/t_pso_archive.c`, `gfx/tests/t_triangle.c`, `gfx/tests/t_pipekey.c`, `gfx/tests/t_pipe_stress.c` |

## Names mentioned in older notes but not read by current code

`NAPPLE_CLEARLOG`, `NAPPLE_EMPTYTRACE`, `NAPPLE_RC_OFF`, `NAPPLE_UBOLOG` and `NAPPLE_WINTRACE` appear only in archived change notes under `docs/archive/`. No source file
reads them. `NAPPLE_PACING_FIXED` and `NAPPLE_PACING_GAME` (gfx pacing) and `NAPPLE_AUDIO_E_PROTOCOL` (audio error code) are C enum constants, not environment variables.
