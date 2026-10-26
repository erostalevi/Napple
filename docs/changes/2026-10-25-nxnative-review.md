# nxnative review: what Napple can take (review only, 2026-10-25)

Branch `claude/nxnative-review-2026-10-25-titles`, session nxnative-review (Mac). **Nothing was implemented, built or run.** No code was copied from nxnative; every claim below cites a file I read, with line numbers where I have them. Paths: `NX:` = `/Users/neto/Documents/Napple/nxnative/` (read-only, commit `1a93086`, MIT, `NX:LICENSE`, Copyright 2026 seedds). Unprefixed paths are in `Napple/` of this repo at main `9ac63a8`. `ACR:` / `PERF:` / `MK8:` = other batch branches read through `git show origin/<branch>`.

Status words: *read* (I read both sides), *inferred* (reasoned from one side, not confirmed on the other), *verify* (needs a run before acting).

## 0. Context that decides how to read everything

1. **Same lineage, different HLE level.** nxnative and Napple descend from the same idea (MacNativeGames / d2r-mobile: run the game's A64 natively, rewrite what macOS forbids, NVN on Metal). They differ below it. nxnative hooks **SDK functions by symbol** (`NX:dm/host/hle_*.c`, `HLE_FORCE(...)`, e.g. `NX:dm/host/hle_sf.c:33`) and runs the SDK's own audio renderer and movie decoder on the CPU. Napple answers **IPC** as the real services would (`docs/runtime-services.md`, `runtime/src/services/`). So nxnative's service and audio fixes are *findings about what games do*, not transplantable code. Their GPU layer is a closer analogue of `gfx/`, but their shaders are translated **offline** by Ryujinx's translator (SPIR-V, SPIRV-Cross, metallib; `NX:dm/gfx/tools/dmsh_build.py`, `NX:dm/third_party/ryujinx/`), ours by an own Maxwell-to-MSL translator (`gfx/src/shader/maxwell_translate.c`, 2044 lines) at run time. Shader findings therefore map by *behaviour*, not by patch.
2. **Nothing from nxnative is in Napple's pipeline yet.** `git grep -il nxnative` on main and on the three batch branches only hits `MacNativeGames/` (a separate snapshot: `MacNativeGames/README.md:258`, `docs/how-it-works.md:84`: its NSP common-ticket import follows nxnative's approach) and my own orchestration file. `git log --all --grep=nxnative` finds only my orchestrate commit. No integrator file or change note cites it. The batch sessions found several of the same bugs independently (marked **overlap** below). So "already in progress" below means *same fix landing from another route*, never "being ingested from nxnative".
3. **MK8D is two different programs.** Evidence from our own logs (session notes, `ios-sim-mk8d-base-a32.log` vs `ios-sim-mk8d-latest-update.log`): base v0 (`b5c39b5b…`) is AArch32 (`a32:` loader lines), but the Testing Material UPDATE v1376256 (`fe941ed5…`) loads as ordinary 64-bit modules (`loader: mapped main … relocs=149788`, no `a32`). nxnative has **no AArch32 support** (no hit for aarch32/armv7/elf32/thumb in `NX:dm/tools`, `NX:dm/host`) and only ever runs the merged 4.0.0 update (`NX:dm/games/0100152000022000/game.tsv` `build v1441792 4.0.0 main=2c336a9b…`, "only the merged 4.0.0 copy is extracted"). Consequence: **base+update+DLC MK8D needs no A32 recompiler**; the A32 work only matters for a base-only build. The update path's first blocker in our runtime is `FAIL: IPC command time:u:300 not implemented` (last line of the update log; see S1).

## (a) Summary and ranked top 15

Short answers to the five questions:

1. **ACNH black frame.** nxnative's AC (3.0.3, same build line) is black unless the profile key `render_cond on` is set: AC wraps clears in `SetRenderEnableConditional`, and "four clear-only passes wiped the composite before the window copy" (`NX:dm/games/01006F8002326000/README.md`, "What it needs"). We record the condition and then draw as if always enabled (`gfx/src/api_cmd.c:435-437`, `gfx/src/mtl_replay.m:1557`). Our timestamp reports already write counter 0 plus time (`gfx/src/mtl_replay.m:225-229`), which is the semantic their evaluation relies on. This is the first thing to try. It may not be the whole story: ac-render's symptom is a *culled* final quad (`ACR` change note, "Next blocker"), whose cause nxnative does not explain (upper-left origin, winding code is equal on both sides: `NX:dm/gfx/nvn/nvn_mtl.m:7366` vs `gfx/src/mtl_replay.m:747-748`). Second suspect from their docs: the quad's data comes from an SSBO and a UBO (driver table words `c0`), see G6/G12.
2. **SMO visual bugs.** nxnative has no note on exploded skinned geometry or the cap brim. Their SMO fixes that touch us: `c0+0x610` largest point size (fog; G6), position invariance (we already have it, `gfx/src/mtl_shader.m:121`), `nvnTextureCompare` channel-mapping fix (HUD). Our integer-vertex-input handling is *ahead* (attr kind keyed per runtime format: `gfx/src/shader/maxwell_translate.c:245,1900`), so the repacked R8_UINT bone-index stream in PERF's finding (`fa3fa316…`) is not explained by their "integer vertex inputs" entry. Best use of nxnative here is as a **reference translator oracle** (E1).
3. **First-use stutter.** They avoid it with (i) shaders compiled offline into metallibs, (ii) a shipped/recorded prewarm manifest of pipeline keys (`pso_manifest.tsv`, `pso_user.tsv`: `NX:docs/development.md:281-293`), (iii) Metal 4 base pipelines specialized in microseconds instead of 3-5 ms builds, with an on-disk archive (`NX:docs/development.md:296-308`), (iv) a look-ahead that compiles the next pipelines of the submission in the background (`pso_predict_rest`, `NX:docs/reference/shaders.md` "The precompile look-ahead restores the scissor"), (v) normalized keys so duplicates collapse (`NX:dm/gfx/nvn/nvn_pso_skey.h:22`). We already persist keys, prewarm and use a binary archive (`gfx/src/mtl_shader.m:1-60`); missing are (ii) a *seed* list for a fresh install, (iv), (iii) and (v).
4. **AArch32.** Not handled by nxnative; see section 0.3.
5. **Where they are more correct / where we are ahead:** see (c) and the "ahead" table in (d).

### Top 15, "take this first" (ranked by benefit to the four titles per effort)

| # | Item (row id) | Titles | Effort |
|---|---|---|---|
| 1 | Evaluate conditional rendering, key `render_cond` (G1) | ACNH, MK8D, TOTK | S-M |
| 2 | `time:u` command 300 and the rest of the MK8D 64-bit boot path; decide base-only A32 is optional (S1, section 0.3) | MK8D | S |
| 3 | Graphics driver-segment words `c0+0x610` (max point size 2047), `+0x644` (base instance) (G6) | SMO, instanced draws everywhere | S |
| 4 | Per-pass dump diagnostics (`PASSDUMP`/`TEXDUMP`/`HILITE`/`DEPTHOVR` style) to pin the AC culled quad and SMO skin bug (T4) | ACNH, SMO | M |
| 5 | Vertex outputs start at (0,0,0,1), not only `pos` (G7) | all | S |
| 6 | Same colour texture at two slots: leave the duplicate unattached (G3) | agl titles (SMO, AC, MK8D use agl G-buffers) | S |
| 7 | Attachment larger than the pass: clear in its own pass first (G2) | all | S |
| 8 | Compressed render target through an uncompressed shadow (G4; queued as backlog "copy-back for compressed RTs") | MK8D | M |
| 9 | `SignalToAddress` type 2 with count <= 0 must write value-2 (K1, our bug) | all | S |
| 10 | Seed prewarm manifest + key normalization + background look-ahead (G11) | all | M-L |
| 11 | Cross-check rewriter jump-table shapes and exclusive-window cases against their list (R1, R2) | ACNH (63 MB text), updates | M |
| 12 | Present path hardening: commit residency before present, `QueueFinish` does not wait for display, present rescue (G10) | windowed Mac, iOS | M |
| 13 | MK8D service stand-ins: six default Miis with valid CharInfo, MiiModel system-data mount, device vs account save as two saves (S2, S4) | MK8D | M |
| 14 | Symptom-indexed reference docs and per-subsystem entries (T1) | process | S-M |
| 15 | CI workflow for host + CPU graphics + tool tests + iOS compile smoke (T2) | all | M |

## (b) Full table

Licence codes: **NX** nxnative's own MIT; **D2R** d2r-mobile MIT (per `NX:README.md` Acknowledgements); **RYU** Ryujinx MIT (`NX:dm/third_party/ryujinx/LICENSE.txt`); **DEKO** deko3d zlib (ported in `NX:dm/gfx/nvn/nvn_tex.c:1-8`); **ATM** Atmosphere GPLv2, **YUZU** GPLv2: reference only, never copy; **MNG** MacNativeGames MIT (`NX:dm/third_party/macnativegames/LICENSE`); **LH** LibHac (version 0.19.0 in `NX:dm/tools/nx_extract/nx_extract.csproj:10`; licence not stated in nxnative's tree, believed MIT upstream, verify before any reuse); **SPV/GLS** SPIRV-Cross, glslang (pinned by tool fingerprint `NX:dm/gfx/tools/dmsh_chain.lock`, not vendored, licences not verified here). Rule for every row: take the *finding and design*, write our own code, cite the nxnative doc in the commit. If any nxnative text or code is ever copied, add it to `THIRD_PARTY_NOTICES.md` (today it lists Ryujinx.Audio, MacNativeGames, LASRecomp, libopus, libvpx; no nxnative).

Owners: `ac-render` owns `runtime/src/services`, kernel waits in `runtime/src`, `gfx/src/api_*.c`, `gfx/src/tex/`; `mk8d-recompiler` owns `runtime/src/a32jit`, `crates/napple-rewrite` A32; `ios-sim` owns `frontend/src/ios`, `runtime/src/ios_*`, `tools/build_ios*`; backlog `smo-gfx` owns `gfx/src/mtl_*.m`, `gfx/src/shader/`. Proposed owners below say which *new* session, so that none touches another's files; rows whose files an active session owns say "after <session> merges".

### GPU: passes, clears, state

| Id | nxnative (file) | Ours (file) | Benefit | Eff / risk | Lic | In progress | Owner |
|---|---|---|---|---|---|---|---|
| G1 | Conditional render evaluated at replay: `NX:docs/reference/rendering.md:551-560`; code `NX:dm/gfx/nvn/nvn_mtl.m:539-575` (counter at rec vs rec+16, timestamp report = counter 0, samples-passed condition keeps drawing), `:1299`, skips at `:8228,8245`; key in `NX:dm/games/01006F8002326000/game.tsv`, `0100152000022000/game.tsv`, TOTK | recorded `gfx/src/api_cmd.c:435-437`; ignored `gfx/src/mtl_replay.m:1557`; timestamp reports right `:225-229` | ACNH black frames, MK8D race clears, TOTK | S-M / SMO: their note says evaluating hurt SMO (`NX:dm/games/0100000000010000/README.md`, "Rendering"), so gate per profile or only evaluate deterministic timestamp conditions | NX (design only) | no | new `gfx-cond` session after smo-gfx (same files `mtl_replay.m`, `api_cmd.c`) |
| G2 | Clear of attachment larger than pass: own pass first: `NX:docs/reference/rendering.md:206-213`, `rp_begin_ex_` `NX:dm/gfx/nvn/nvn_mtl.m:2707` | load-action clears folded when `clear_covers_target()` `gfx/src/mtl_replay.m:328-350,504` (size mismatch handling *verify*) | stale depth/colour outside small passes (agl small targets) | S / low | NX | no | smo-gfx |
| G3 | Same texture at two colour slots: unattach duplicate: `NX:docs/reference/rendering.md:185-191`, `rt_dup_slot` `NX:dm/gfx/nvn/nvn_mtl.m:2673,2725` | no equivalent found (grep for dup/same texture in `mtl_replay.m`: none) | G-buffer corruption on TBDR (scattered squares); agl titles bind such slots | S / low | NX | no | smo-gfx |
| G4 | Compressed RT via shadow: `NX:docs/reference/rendering.md:170-177`, `bcrt_attach/bcrt_flush` `NX:dm/gfx/nvn/nvn_mtl.m:2571-2603`; depth-via-colour shadow `shrt_block` `:2603` | backlog item "copy-back for compressed render targets (MK8D)" `orchestrations/backlog.md` | MK8D | M / medium | NX | queued, not started | smo-gfx |
| G5 | Layered passes for layer-writing VS: `NX:docs/reference/rendering.md:199-205` | no `renderTargetArrayLength`/layered path found in `gfx/src/mtl_replay.m` | LUT/cube passes (not seen on our 4 titles) | M / low | NX | no | backlog |
| G6 | Driver segment `c0+0x610` = 2047 (max point size, SMO `alPointSpriteShader`), `+0x644` base instance: `NX:docs/reference/shaders.md:505-519`, `NX:dm/gfx/nvn/nvn_mtl.m:5234-5238`, `:4990`; layout table `NX:docs/agents/04-graphics.md` section 3 | graphics `c0` carries only SSBO tables at 0x110/0x510 `gfx/src/mtl_replay.m:790-796`; compute words 0x200/0x210/0x310 are present `:801-806` (same layout as theirs) | SMO Cap fog never parts; any shader using `gl_BaseInstance` | S / low | NX | no | smo-gfx |
| G7 | Vertex outputs init (0,0,0,1): `NX:docs/reference/shaders.md:323-329`, `vs_init` `NX:dm/gfx/tools/dmsh_build.py:580` | only `out.pos` set, varyings zero: `gfx/src/shader/maxwell_translate.c:1956` | LAS sun shadows were wrong without it; unwritten w=0 divides | S / low | NX | no | smo-gfx |
| G8 | (diagnostics are listed as T4) | | | | | | |
| G9 | Lower-left origin by negative viewport + reversed winding + flip at present `NX:dm/gfx/nvn/nvn_mtl.m:3529,7349,7366,8391` | rows stored bottom-up, flipped at present `gfx/src/internal.h:40`, `mtl_replay.m:370,483,723-759`, `mtl_present.m:363`; LAS title text reads correctly in `ios-sim-las-title.png` (session notes) | none now | n/a | | equivalent, keep | none |
| G10 | Present hardening: residency committed before present `NX:docs/reference/rendering.md:279-286`; `QueueFinish` waits GPU only `NX:docs/reference/nvn-objects.md:165-172`; present rescue `NX:docs/reference/frame-rate.md:50-57`; present snapshots `:70-82`; dynamic interval holds `:59-69` | `gfx/src/mtl_present.m` (not read in full: *verify* each) | windowed Mac, iPhone smoothness | M / low | NX / D2R | no | `gfx-present` |
| G11 | Pipelines: Metal 4 base + specialization + archive files `NX:docs/development.md:296-308`; seed manifest + user manifest `:281-293`; look-ahead `NX:docs/reference/shaders.md` "precompile look-ahead"; key normalization `NX:dm/gfx/nvn/nvn_pso_skey.h:22` | persisted keys, background prewarm, binary archive, wait-or-build `gfx/src/mtl_shader.m:1-60,610`; archive off on iOS Simulator `:52` | first-visit stutter (`mtl_pipe_get`), cold-start | M-L / medium (Metal 4 path is Mac-only in theirs; iOS stays off) | NX | no | smo-gfx (same file) |
| G12 | Inline uniform updates replayed as ordered GPU copies, barriers inside encoders, alias visibility: `NX:docs/reference/rendering.md:459-482` | CPU `memcpy` at replay `gfx/src/mtl_replay.m:1577`, ring copy per draw `:830-` (ordered on the CPU timeline; wrong only if the buffer is also GPU-written earlier) | AC final pass reads UBO+SSBO; *verify* with a pass dump | M / low | NX/MNG | no | `gfx-cond` |
| G13 | `fp` math: `precise::` min/max/clamp, iOS relaxed math: `NX:docs/reference/shaders.md:338-350` | our FMNMX uses `fmin/fmax` `maxwell_translate.c:1057-1059`, `fastMathEnabled = NO` `mtl_shader.m:122`, no iOS math-mode flag *verify* | NaN lighting (white half on Mario in Galaxy 2) | S / low | NX | no | smo-gfx / ios-sim |
| G14 | `SR_Y_DIRECTION`: open on their side (`NX:docs/reference/shaders.md:385`, Ryujinx returns +1) | we return -1 always `maxwell_translate.c:1423` | LAS-like shaders under upper-left origin | S, *verify* | | no | smo-gfx |
| G15 | Geometry shaders as compute + capture: `NX:docs/geometry-metal-research.md`, `NX:dm/gfx/tools/dmsh_geometry.py`, `NX:dm/gfx/tests/t_geometry.m` (498 checks); tessellation not drawn (`NX:docs/reference/shaders.md:464`); ACNH keyboard has 8 GS, SMO 3 (thunder), MK8D 1: none translated there either | none; backlog `gfx-features` (L) | low for the 4 titles | L / medium; their path rests on Ryujinx's GS lowering, ours has none: design only | RYU (design) | backlog | later |
| G16 | Image atomics contract `NX:docs/image-atomic-binding-contract.md`, bounded MSL 2.3 emulation `NX:docs/reference/shaders.md:615-676` | the brief says we have `docs/image-atomic-binding-contract.md` and `docs/geometry-metal-research.md`: **neither file exists in this checkout or on any branch** (`git log --all --name-only`); backlog `shader-surface` | SULD/SUST titles (not our 4) | L | NX | backlog | later |
| G17 | Texture details: new textures zero `NX:docs/reference/rendering.md:15`, cube array = layer-faces `:65` (**overlap**: ACR commit `22752de`), block-height truncation `:108`, sparse `:117`, 1D as 2D `:51`, buffer-texture row wrap `:93`, stencil carried in depth-stencil copies `:542`, `copy_t2t_draw` for RG11B10F to RGBA16F `:529` | `gfx/src/tex/napple_layout.c` (same packaged-layout byte as theirs `:4`), sparse getters are stubs `gfx/src/api_texture.c:72,147` | per-title bugs not yet seen | S each | NX/DEKO (layout origin: both cite deko3d zlib `NX:dm/gfx/nvn/nvn_tex.c:1-8`; check our file header) | partly | `gfx-tex` after ac-render |
| G18 | Depth16 clear rounding `NX:dm/gfx/nvn/nvn_depth_clear.h:8`, partial colour/depth clears as triangles `NX:dm/gfx/nvn/nvn_mtl.m:8105` | clear quads `gfx/src/mtl_replay.m:512-606`; index != 0 falls back to a pass restart `:547` | partial clears on MRT | S-M | NX/MNG | no | smo-gfx |
| G19 | Same-pass feedback via snapshot for listed shaders `NX:docs/reference/rendering.md:260-270`, `fb_prepare` `NX:dm/gfx/nvn/nvn_mtl.m:4939` | none | magenta 16x16 blocks on A-series GPUs (iPhone) | M / perf cost, per-profile list | NX | no | ios later |

### Kernel and threads

| Id | nxnative | Ours | Benefit | Eff / risk | Lic | In progress | Owner |
|---|---|---|---|---|---|---|---|
| K1 | `SignalToAddress` type 2, count <= 0 with waiters: value-2 (`NX:docs/reference/threads.md:61-66`, mesosphere `KAddressArbiter`; Ryujinx and yuzu agree) | writes value-1: `runtime/src/threads.c:566-570` (`cnt <= 0 ... val - 1`); count>0 branch matches | condition variables / mutexes in every title | S / low | ATM (reference only; implement from the documented semantics) | no | after ac-render (owns kernel waits) |
| K2 | `WaitForAddress` type 3 (64-bit wait-if-equal, SDK 19+) `NX:docs/reference/threads.md:67-73` | types 0-2 only, type 3 returns invalid enum `threads.c:546-552` | none of our 4 | S | ATM | no | later |
| K3 | Same-core handoff when a higher-priority thread wakes/starts on the caller's core: `NX:docs/reference/threads.md:7-14,43-51`, `dm_yield_to_core_peers` in `NX:dm/host/hle_thread.c` | we model three virtual cores explicitly (`runtime/src/threads.c:16,253`, `NAPPLE_FREE_CORES`); may already give this: *verify* with an equivalent test (`NX:dm/host/tests/t_preempt*`) | removes racy guest crashes (LAS physics, ACNH loaders) | M / medium | ATM semantics | no | after ac-render |
| K4 | Yields that make progress return at once (`thr_yield` `NX:dm/host/hle_thread.c:660`, `NX:docs/reference/threads.md:101-109`): ACNH shows a black plane-icon screen for 77 s without it | our yield hands the core to an equal-priority waiter, else `sched_yield` `runtime/src/threads.c:367-373`; no back-off to remove | ACNH loading time: probably not affected (no back-off), *verify* once it renders | S | NX | no | none |
| K5 | `sleep_precise` (3/4 of remaining per step) `NX:docs/reference/threads.md:86-100` | our SleepThread implementation not read | 30 Hz title pacing (27 Hz measured on their side) | S, *verify* | NX | no | later |
| K6 | `svcGetInfo` FreeThreadCount = 96 minus live `NX:docs/reference/threads.md:28-35` | constant 0x100 `runtime/src/kernel.c:208` | Unity only | S | ATM | no | none |
| K7 | Thread pause + context for GCs `NX:docs/reference/threads.md:36-42` | implemented `runtime/src/threads.c:414,449`; `docs/runtime-services.md` still says "existence check only" (doc stale) | none | docs only | | | |
| K8 | `SetThreadCoreMask`: ideal core moved into the new mask `NX:docs/reference/threads.md:15-20` | returns InvalidCombination if ideal not in mask, ideal -3 checks current `threads.c:390-405` | cannot tell which matches Horizon; Atmosphere reference needed | S, open question | ATM | no | see (e) |

### Services, input, storage, audio, movies (findings, not code)

| Id | nxnative | Ours | Benefit | Eff / risk | Lic | In progress | Owner |
|---|---|---|---|---|---|---|---|
| S1 | (their `time::` hooks, `NX:docs/reference/services.md:508-535`: steady clock, snapshots, user clocks) | `time:u` table ends at cmds 0-5,20,100-102,200,400,401,500,501 `runtime/src/services/time.c:185-199`: **no 300** (CalculateMonotonicSystemClockBaseTimePoint per switchbrew; confirm) | MK8D update boots until here (ios-sim log) | S | none | no (found in our logs, not from nxnative) | ac-render's area (services): after merge, or tiny `mk8d-boot` session |
| S2 | MK8D Mii: six default Miis with valid CharInfo, random for `BuildRandom`, Mii editor cancelled; MiiModel system data `0100000000000802` mounted as `MiiResource:` with a stand-in archive (layout in `NX:dm/games/0100152000022000/README.md` "Miis"; `NX:dm/host/hle_mii.c:137`) | `runtime/src/services/mii_core.c:21-42` has default Miis; system-data mount: only `OpenDataStorageByDataId` for DLC `fs.c:553-572` (*verify* MiiModel) | MK8D crashes at boot without both | M / low | NX | no | MK8D follow-up |
| S3 | Device and account save as two saves when a game mounts both `NX:docs/reference/services.md:163-169`, `NX:dm/host/hle_fs.c:81`; MK8D deletes `CommonSave:/userdata.dat` at boot | save types are separate directories by type `docs/runtime-services.md` "Save data"; *verify* MK8D | MK8D re-asks Mii every boot if shared | S | NX | no | MK8D follow-up |
| S4 | AOC mount cache size = RomFS working memory `NX:docs/reference/services.md:186-192`, `romfs_work_size` `NX:dm/host/hle_fs.c:347` | arises from SDK-level hooks; at IPC level likely N/A | MK8D DLC | S, *verify* | NX | no | MK8D follow-up |
| S5 | Game's own HIPC clients (`olsc:u`, `npns`) answered `NX:docs/reference/services.md:50-56`, `NX:dm/host/hle_sf.c:33` (ACNH aborts without) | `olsc` exists (`services/olsc.c`), `npns` not in `runtime/src/services/`; manager enforces the NPDM access list | ACNH after first frames | S, *verify* with `NAPPLE_IPC_LENIENT=1` | NX | no | ac-render |
| S6 | History reads fill all 16 entries with consecutive numbers (SMO sticks dead otherwise) `NX:docs/reference/services.md:200-209`; six-axis sensors per controller and shake `:217-224` | `input/src/hid_shm.c:148` count capped to 16; outer=2n, inner=n fix (**overlap**: ACR); six-axis rings exist `:44,87`; shake *verify* | SMO sticks/motion, cap throw | S | NX | partly | ac-render / input |
| S7 | Audio renderer details (SDK 4/6/8 DeviceSink ids, update lock) `NX:docs/reference/audio.md:38-100` | N/A at IPC level (`audio/`, `services/audio.c`) | none | none | | | |
| S8 | Movies: 16 video output slots `NX:docs/reference/movies.md:109-115`; VP9 via libvpx, Vorbis via stb_vorbis, H.264 via VideoToolbox; vendored in `NX:dm/third_party/` (BSD-3 / MIT-or-PD) | ours uses `runtime/src/movie.c`, `vt_h264.c`, Homebrew libvpx (`THIRD_PARTY_NOTICES.md`); ios-sim added libvpx for the Simulator (commit `5fb4673`) | ACNH opening movie (VP9 1080p + Vorbis) | M | NX/third-party | ios-sim (libvpx) | ios-sim |
| S9 | Frame pacing / present interval handling `NX:docs/reference/frame-rate.md` | `runtime/src/timescale.c` etc. not compared | smoothness | M | | | later |

### Loader, rewriter, ingest, memory

| Id | nxnative | Ours | Benefit | Eff / risk | Lic | In progress | Owner |
|---|---|---|---|---|---|---|---|
| R1 | Jump-table shapes (7+): `NX:docs/reference/rewriter.md:23-100`; halfword tables padded to the largest entry, patch area 42 MB to 7.9 MB for ACNH `:134-143`, ACNH note `NX:dm/games/01006F8002326000/README.md` "Rewriter" | `crates/napple-rewrite/src/jumptab.rs:18-24` (3 shapes + function-span fallback), `rewrite.rs:198` "landing pads too far"; ACNH data builds today | ACNH-size modules (63 MB text), game updates | M / low | NX | no | `rewrite-xcheck` (disjoint from mk8d-recompiler's A32) |
| R2 | Exclusive windows: exit order, out-of-line retry blocks, adrp in window `NX:docs/reference/rewriter.md:103-132` | `crates/napple-rewrite/src/windows.rs`, `check.rs` | atomics correctness (job systems hang) | M, mostly tests | NX | no | `rewrite-xcheck` |
| R3 | Spin-wait rewrite for power `NX:docs/development.md:310-320`; `NX:dm/tools/patch_nso.py` | none | battery/thermals (iOS) | L / medium | NX | no | ios later |
| R4 | Per-build workarounds keyed by build id, `MATCH_CUTS` function fingerprints `NX:docs/reference/rewriter.md:7-22`, `NX:dm/tools/nso_match.py` | per-build signatures `knownSignatures/` | updates keep a workaround | M | NX | no | later |
| R5 | Extraction: LibHac `nx_extract` `NX:dm/tools/nx_extract/Program.cs` (231 lines) | own Rust: `crates/napple-core`, `napple-build/src/bktr.rs`, NCZ support; nothing to take (ours is ahead: NSZ, no .NET) | none | none | LH | | |
| R6 | Survey + provisional profile for unknown titles (`NX:dm/games/profile_guess.py`, `NX:scripts/build_ipa_from_nsp.py`, `NX:docs/agents/02-survey.md`) | Studio/CLI `napple build`, no survey step *read-only check* | generality, coverage | M-L | NX | no | later |
| R7 | Guest memory layout (4 GB layout, `sys_resource`, heap from pool minus used) `NX:docs/reference/memory.md:7-50`, ACNH values in its game.tsv (`mem_reserve 0x100000000`, `app_pool 0xCD500000`, `mem_used 0x7a00000`) | `runtime/src/memory.c` layout; iOS budget is backlog `ios-memory` | iOS budget | M | NX | backlog | ios-memory |

### Tooling, tests, docs

| Id | nxnative | Ours | Benefit | Eff / risk | Lic | In progress | Owner |
|---|---|---|---|---|---|---|---|
| T1 | Symptom index + entries "Symptom, Cause, Fix, Found on" per subsystem; `NX:docs/agents/symptoms.md`, `NX:docs/reference/*.md`, `NX:dm/tools/docs_symptoms.py`, `NX:dm/tools/tests/test_docs.py` (links, anchors, length limits) | `docs/changes/*` dated notes; no symptom index | faster bring-up of the next title | S-M | NX | no | docs session |
| T2 | CI `NX:.github/workflows/tests.yml` (host + CPU graphics tests, TSAN targets for audio/memory, iOS compile smoke `NX:dm/ios/tools/build_smoke.py`) | `.github/workflows/` has pages.yml and release.yml only | regression safety | M | NX | no | `ci` session |
| T3 | ~40 GPU regression tests `NX:dm/gfx/tests/t_*.m` (clear_ds, clear_tex, depth16, feedback, present_snap, pso_archive, geometry, copy_alias ...) | `make -C gfx test` | test ideas for G1-G4, G18 | S each | NX | no | with each gfx row |
| T4 | Diagnostics: `DM_NVN_PASSDUMP`, `TEXDUMP`, `CBDUMP`, `HILITE`, `DEPTHOVR`, `KEYDUMP`, `MAGSCAN`, `GPUPROF` `NX:docs/agents/04-graphics.md` section 7 | `NAPPLE_DRAWLOG*`, `NAPPLE_SHADER_PROBE`, `NAPPLE_DEBUG_SOLID_FS`, `NAPPLE_DEBUG_SKIP_VS` (`gfx/src/mtl_shader.m:470-500`, PERF note) | pass-level dump with clears and per-pass render-condition state would have shown the AC final pass | M | NX (design) | no | smo-gfx |
| T5 | iOS build and launcher settings table `NX:dm/ios/tools/*`, `NX:dm/games/settings.tsv` model, `ios_metal.py` relaxed math | `tools/build_ios*.sh`, `docs/ios.md` | | M | NX | ios-sim | ios-sim |
| T6 | Per-title notes with "what it needs / open" (`NX:dm/games/<id>/README.md`, 65 titles; ours under `compat/`, `knownSignatures/`) | | | S | | | |

## (c) Discrepancies: our behaviour contradicts theirs (candidate bugs on our side)

| # | Ours | Theirs | Evidence | Confidence |
|---|---|---|---|---|
| c1 | Conditional render always drawn, warning "treated as always enabled" | Evaluated; AC and MK8D require it, SMO wants it off | `gfx/src/mtl_replay.m:1557` vs `NX:dm/gfx/nvn/nvn_mtl.m:539-575`, AC/MK8D `game.tsv` | high for AC/MK8D semantics; whether it explains our culled quad: unknown |
| c2 | `SignalToAddress` type 2, count <= 0, waiters: value-1 | value-2 | `runtime/src/threads.c:568` vs `NX:docs/reference/threads.md:61-66` | high (documented kernel behaviour) |
| c3 | Graphics `c0` words 0x610 and 0x644 left 0 | 2047 and base instance | `gfx/src/mtl_replay.m:790-796` vs `NX:dm/gfx/nvn/nvn_mtl.m:5234-5238` | high |
| c4 | Varyings start at 0 | (0,0,0,1) | `gfx/src/shader/maxwell_translate.c:1956` vs `NX:dm/gfx/tools/dmsh_build.py:580` | high |
| c5 | `time:u` has no cmd 300 | their host answers the SDK `time::` calls | `runtime/src/services/time.c:185-199`, ios-sim update log | high |
| c6 | `docs/runtime-services.md` says SetThreadActivity is an "existence check only" | implemented | doc vs `runtime/src/threads.c:414` | docs stale |
| c7 | Window origin lower-left: our targets stored bottom-up, flipped at present | viewport flipped, rows top-down | `gfx/src/internal.h:40` vs `NX:dm/gfx/nvn/nvn_mtl.m:3529` | both work for LAS; keep, watch MIRROR_Y copies |
| c8 | `SR_Y_DIRECTION` = -1 | their open question (Ryujinx +1) | `maxwell_translate.c:1423` vs `NX:docs/reference/shaders.md:385` | unknown |
| c9 | No unattach of duplicate colour slot | unattached | none vs `NX:dm/gfx/nvn/nvn_mtl.m:2673` | medium |

## (d) Do NOT take, and where we are ahead

Do not take:
- **Any Atmosphere / yuzu code**: GPLv2, reference only (K1-K3). Implement from documented semantics.
- **Ryujinx translator and dotnet toolchain** as a runtime dependency: wrong architecture (offline SPIR-V chain vs our run-time translator), .NET at build, large per-game library sets (up to 344 MB on iOS for MH Rise per their note). Use it only as an offline *oracle* (E1).
- **SDK-symbol HLE** (`HLE_FORCE`, in-process audio renderer, movie decoder hooks, `hle_sf.c`): we answer IPC; copying hooks would fork our model.
- **Per-game profile TSV system and per-build rewriter cuts** as a first move: our rule is data-only per-game knowledge via `knownSignatures`; adopt only the *findings*.
- **LibHac `nx_extract`**: ours is native Rust and ahead (NSZ, no .NET).
- **iOS-only items** now (power QoS, spin-wait rewrite, present snapshots): out of scope until iOS device work (backlog says iOS out of scope).
- **Geometry/tessellation implementation**: depends on the Ryujinx GS lowering; none of the 4 titles needs it to reach gameplay (SMO thunder only).
- **Their vendored copies**: keep our own provenance. Anything adapted needs a `THIRD_PARTY_NOTICES.md` entry.

Where we are ahead (nobody copy backwards):
- Vertex integer inputs keyed per runtime attribute format, no offline guess and no shadow libraries (`gfx/src/shader/maxwell_translate.c:245,1900`); stride-not-multiple-of-4 repack, fans/quads/8-bit indices exist (`mtl_replay.m:721,1050,1108,1179`).
- Depth mode and origin resolved per draw in the pipeline key, no second entry point per vertex library (`mtl_replay.m:724`, `mtl_shader.m:463`).
- Compute `c0` dispatch size and UBO/SSBO tables already match their layout (`mtl_replay.m:801-806`), as does `preserveInvariance` (`mtl_shader.m:121`).
- Own AArch32 route in flight (A32 interpreter + lifter; they have none). Own NSZ/BKTR ingest in Rust. Transactional save data with crash-safe commit (`docs/runtime-services.md` "Save data"). SetThreadActivity/GetThreadContext3 implemented.
- IPC-level services with an NPDM access list: SDK-version differences do not matter, which is why their SDK 4/6/8/20 audio-command fixes do not apply.

## (e) Open questions

1. Does evaluating `render_cond` fix the ACNH black image, or only the culled-quad bug remains? Run ac-render's build with `NAPPLE_DRAWLOG` and a pass dump of the last pass (conditional state, clears) before changing code. AC (their note) needs it by frame 900; ours draws ~1200 frames in 45 s.
2. Why is our final-pass quad culled when upper-left winding code is equal? Candidates: vertex data from the SSBO (c0 table, `gpu_va`), UBO projection, front-face sense after `SR_Y_DIRECTION`, `depth_mode` clip. Needs a CPU evaluation of one vertex.
3. SMO skinned geometry: is our translation of fa3fa316 numerically equal to Ryujinx's? Proposed oracle: build `NX:dm/third_party/ryujinx` offline (MIT) on the same shader bytes, compare outputs for random inputs. Is that within what the user wants (their repo, `Do not run builds of nxnative` applied to this review only)?
4. MK8D: confirm with a run that update v1376256 + base + DLC reaches the title through the 64-bit path after `time:u:300`; if so, deprioritise the A32 lifter for the user's goal. (All MK8D evidence here comes from logs, not from a run of mine.)
5. Brief says we have `docs/image-atomic-binding-contract.md` and `docs/geometry-metal-research.md`; they are not in this checkout or any branch. Are they on an unmerged local branch?
6. `SetThreadCoreMask` when ideal core -3 and the current ideal is outside the new mask: Horizon result vs ours (K8)? Needs the Atmosphere source, not read here.
7. LibHac / SPIRV-Cross / glslang licences are not in nxnative's tree; confirm upstream before any reuse.

## Method and limits
Read in full: `NX:dm/games/{01006F8002326000,0100000000010000,01006BB00C6F0000,0100152000022000}/README.md`, `NX:AGENTS.md`, `NX:docs/agents/04-graphics.md`, `NX:docs/reference/{threads,rendering(137-290,329-560),shaders(1-20,301-520,697-741),rewriter(1-135)}.md` parts, `NX:docs/development.md` (52-73, 281-334), `NX:dm/third_party/README.md`, game.tsv of MK8D. Headings only: remaining reference docs. Spot-read code: `NX:dm/gfx/nvn/nvn_mtl.m` at the cited lines. Ours: cited files at cited lines only; large files (`mtl_present.m`, `tex/`, `a32jit`) not audited. No builds, no game runs. Sibling notes used: `ac-render-progress.md`, `ac-303-stall-findings.md`, `smo-title-screen-visual-bugs.md`, `mk8d-is-aarch32.md`, ios-sim logs (all in the shared notes folder), `ACR`/`MK8`/`PERF` change notes via `git show`.
