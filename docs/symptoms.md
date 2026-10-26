# Symptom index

Look up what you see, find the known cause and fix. Format: one row per symptom: **Symptom**, **Cause**, **Fix**, **Found on** (title or area, change note).
Add a row when you find a new one (newest at the bottom of its section). Per-subsystem entry points are in [reference.md](reference/subsystems.md); per-title status is in [titles/](titles/README.md).
`tools/check_docs.py` checks that every link and anchor in these files resolves.

## Boot, crashes, hangs

| Symptom | Cause | Fix | Found on |
|---|---|---|---|
| Boot thread spins at 25 M `WaitSynchronization(handle 0, timeout 0) -> 0xe401` per second, nothing renders | `IApplicationFunctions` cmd 210 was a guessed `CreateMovieMaker` returning an object; the real reply carries a copy handle to an event, so the guest kept handle 0 | cmd 210 returns a copy handle to a never-signalled event (`runtime/src/services/applet.c`). Proof: `NAPPLE_TRACE_IPC=all` shows the spin right after `ICommonStateGetter::GetOperationMode` | Animal Crossing, [ac-boot-spin](archive/changes/2026-10-24-ac-boot-spin.md) |
| Abort "request on handle 0" in `nn::vi::Initialize` (sdk+0x447b30), Break at sdk+0xc80730 | TIPC `sm` refusal reply (services outside the NPDM list: `vi:m`, `vi:s`, `pctl:*`) was header plus result only; the SDK reads move handle and result at the success positions, found the request bytes there, and treated the service as available | `sm_tipc_error_layout()` in `runtime/src/ipc.c` puts a move handle 0 into the error reply. Test: `runtime/tests/test_ipc.c` | Animal Crossing 3.0.x, [guest-handle](archive/changes/2026-10-24-guest-handle.md) |
| ACNH 3.0.3 boots but draws 0 frames, thread sits in sdk+0x5087xx | The SDK's Npad lifo reader retries forever on an odd outer sampling number; our HID writer produced 0,1,2,3... Real HID outer numbers are even | `input/src/hid_shm.c`: outer = 2n, inner = n (`input/tests/test_hid.c` expects it). Check with `NAPPLE_PCPROF=1` | Animal Crossing 3.0.3, ac-render (integrator file `claude-ac-render-2026-10-25-titles`) |
| `CRASH signal=11 fault=0x38` right after the `pl:u` opens, intermittent ("unreproducible boot crash") | Save root without `system/fonts`: the guest got zero-size shared fonts | `pl.c` serves a built-in placeholder TrueType font when no real font is found (prints one actionable line). Install real fonts into `<save-root>/system/fonts` for text | LAS, SMO, [fonts-fallback](archive/changes/2026-10-24-claude-fonts-fallback-2026-10-24-plan7.md), [verify-media-dlc](archive/changes/2026-10-24-verify-media-dlc.md) |
| MK8D 1.0 (AArch32) faults at main+0x562d00 / 0x822994 (`this = NULL + 0x3e74`) around frame 175 | Mii default CharInfo did not have a UUID v4 create id, so `CharInfoAccessor::IsValid` failed, MK8D never built its Mii head-model singleton and a worker thread dereferenced it | Default Miis are UUID v4 (`services/mii_core.c`, commit 76249e5); `tests/test_svc_mii.c` follows | MK8D, mk8d-recompiler branch note `2026-10-25-mk8d-recompiler` |
| MK8D AArch32 interpreter crashes in `SharedMemoryImplByHorizon::Map` | Main thread stack (0x20000000) lay in a Free gap; the SDK's address-space scan put the `pl:u` shared fonts (0x1100000 bytes at 0x1fe04000) over it | Reserve the main stack for `QueryMemory` (`runtime/src/a32/a32_proc.c`) | MK8D 1.0, mk8d-64 branch note `2026-10-26-claude-mk8d-64-2026-10-25-titles` |
| AC v0 SIGSEGV about one run in three | Command-buffer recording lists were touched by several threads without one lock; finalized or null command buffers dereferenced | Recording lists under one lock, null/finalized buffers tolerated (`gfx/src/api_cmd.c`); 0 crashes in 9 runs | Animal Crossing v0, ac-render |
| AC shows the real date as 1970 / boot waits on `olsc` / `prepo` | The game's clock read epoch 0; `olsc:u` cmd 13 must succeed; `prepo:u` 10104/10105 must succeed | AdjustableUserSystemClock override in `services/time.c`, called from `main.c`; the olsc and prepo handlers | Animal Crossing 3.0.3, ac-render |
| `nvnProgramSetShaders` with a tiny pool-relative address (0xe00) crashes | The game passes code offset 0xe00 once | Reject pool-relative shader addresses instead of dereferencing (`gfx/src/api_program.c`) | Animal Crossing, ac-render |
| `TextureInitialize: storage out of range (pool .. off .. size ..)` | CUBEMAP_ARRAY layout used depth = layers, but the game's depth is layers x 6 faces; pool allocated for fewer | Cube-array depth = layer-faces (`gfx/src/tex/napple_layout.c` and the one line in `mtl_tex.m`). MK8D still logs a similar one for another texture | Animal Crossing; MK8D open |
| iOS Simulator: abort in gfx for 48 bytes of colour attachment | The Simulator allows 32 bytes per pixel of render-target storage; MK8D's G-buffer pass needs 48 | Open: needs a gfx fallback (split the pass or drop attachments) | MK8D on iOS Simulator, [ios](architecture/ios.md) |
| LAS aborts in the movie library on iOS | Needs more than the 3.25 GiB device heap cap; the Simulator uses 4 GiB | Open (decision: trim the low gap or require the increased-memory entitlement) | LAS on iOS, [ios](architecture/ios.md) |
| Depth textures crash on iOS | Depth textures must not be Shared storage on iOS | `gfx/src/mtl_tex.m` uses Private on iOS | iOS Simulator, ios-sim branch |

## Rendering

| Symptom | Cause | Fix | Found on |
|---|---|---|---|
| ACNH presents a black image although frames are drawn | The final pass into the window texture was culled or mis-rendered (winding for the upper-left origin), later a never-sampled composite not copied into the window | Stopgap `mtl_frame_present_pick` presents the composite target (disable with `NAPPLE_NO_ORPHAN=1`); root cause (no op copies the composite into the window, pool slot 1634) open. ACNH shows its game with it | Animal Crossing 3.0.3, ac-render |
| SMO exploded geometry (huge slabs/wedges), parrot after ~2 min idle, shattered cap brim and white ribbons on the title menu | The command-list graveyard released recordings idle for > 30 s (`GRAVE_IDLE_S`); the game calls those handles again, `CallCommands: stale or invalid command handle ... ignored`, so display lists run without their state | Recordings are kept until a byte budget (`NAPPLE_REC_BUDGET_MB`, default 1024) instead of idle time (`gfx/src/api_cmd.c`, ac-render). Proof: A/B at 215 s with `GRAVE_IDLE_S` 30 vs unlimited, see the nx-shader branch note `2026-10-25-nx-shader` | SMO, nx-shader |
| `CallCommands: stale or invalid command handle` 65 k+ per minute | MK8D creates about 4000 lists/s and never finalizes them; the recording budget is exceeded | Open (needs a smarter keep-alive: do not release unfinalized handles the game can still call) | MK8D |
| SMO half-scale duplicate view in the top-left ~53 % of the frame | With render scale on, a pass mixed attachments: colour 0 is a 2D array (never scaled) next to scaled siblings and depth; Metal renders only the overlap | `pass_unify_scale` (`mtl_replay.m`) and `mtl_tex_force_scale` (`mtl_tex.m`) lift unscaled same-size attachments. Find similar with `NAPPLE_SCALE_DIAG=1` | SMO, perf-las-live |
| Skinned draw reads garbage bones / neighbouring memory | Register-indexed constant-buffer reads past the bound size read the ring memory; hardware returns 0 | `cbr(cb, size, off)` with the sizes table at `[[buffer(30)]]`, translator version 7 (`gfx/src/shader`) | SMO |
| Comparison `a != b` true for NaN, wrong branch in translated shaders | Maxwell's ordered NE is false when either operand is NaN; MSL `!=` is true | Emit `(a < b) \|\| (a > b)` for FSETP/FSET/FCMP NE; `fmin/fmax` as `precise::`. `NAPPLE_GFX_MATH=safe\|relaxed\|fast` | nx-shader (G13), `t_xlate` |
| Shader build fails "unknown instruction 3b34056060470100" | `VMNMX` (video min/max) not translated | Implemented in full in the translator (`maxwell_translate.c`) | AC v0 fragment `23e43e94e45bb1d5` |
| Stutter of 170-1300 ms when entering a new area | Pipelines and shaders built synchronously on first use (`mtl_pipe_get`) | Persisted keys (`pipelines.bin`) survive translator bumps, seed manifests (`NAPPLE_GFX_PIPE_SEED`), key normalisation; opt-in speculation `NAPPLE_GFX_SPECULATE=1`. A warm second visit is smooth | LAS, SMO, perf-las-live, nx-shader |
| RSS 2-3.4 GB growing, `Malloc Small` 2 GB | Command-list graveyard released at most 256 recordings per second | Release everything idle each pass (`api_cmd.c`); watch `cmdrecs live` in `NAPPLE_PERF_LOG=1` | LAS, perf-las-live |
| MK8D stops at about 45 fps headless | Guest blocked in SyncWait ~21 ms per present, per-submit commit to complete ~21 ms, present GPU time <= 8 ms, replay CPU 0.7 ms: gfx-bound (LAS runs 58 fps) | Open. Headless pacing as a timer in `mtl_present.m` gave 46 to 60 on one run (see ac-render integrator file) | MK8D |
| Render target view of a block-compressed texture (format 0x44) shows stale data | Compressed render-target shadow is not copied back | Open (G4) | MK8D |

## Services and input

| Symptom | Cause | Fix | Found on |
|---|---|---|---|
| Title shows "Ver. 1.0.0" for an updated game | `GetDisplayVersion` always returned 1.0.0 | Display version from `napple-app.json` (NACP) when the data directory is the newest version (`services/host_info.c`) | MK8D update |
| `time:u` cmd 300 not implemented, MK8D latest update stops | Missing `CalculateMonotonicSystemClockBaseTimePoint` | Implemented on the ios-sim and mk8d-64 branches; both add it to `services/time.c`: keep one handler (conflict at integration) | MK8D update |
| Updated game: save files appear as a different title's | Base and update share one title save dir on purpose (they are one title) | Not a bug. Device vs account saves are mounted separately (`save/space1/device`, `save/space1/account/<uid>`) | MK8D |
| Out of disk building an updated title (19 GB RomFS) | The patched RomFS is materialised | `napple build ... --data-only --update-romfs in-place` writes `romfs.map` (268 MB for MK8D). Never ship a map: it holds keys and host paths | MK8D, mk8d-64 |
| zsh: `NAPPLE_INPUT_SCRIPT="..:$t:A"` turns into a path | `$t:A` is a zsh modifier | Quote or escape `$`; write `15:A,19:A,...` literally | iOS Simulator work |
| `pgrep -f napple-runtime` finds other sessions' processes | Same binary name | Include the data directory in the pattern; never `pkill` by name | all |

## Building

| Symptom | Cause | Fix | Found on |
|---|---|---|---|
| codesign fails on a build under `~/Documents` | File Provider extended attributes | Build in `$TMPDIR` as the scripts do, or `xattr -cr` | packaging |
| `make -C runtime` link fails on a test after adding a debug trace | Guest-stack trace aids referenced from runtime code but not linked in the host tests | Keep trace aids out of files the host tests link, or stub them | ac-render |
| iOS build misses `a32` symbols | `tools/build_ios.sh` did not compile `runtime/src/a32/*.c` | Fixed on the ios-sim branch | iOS |
| `dylibize` fails on an AArch32 (NTB2) title | NTB2 modules are not Mach-O liftable | Skipped for NTB2 | iOS, MK8D |
