# Feature support

Coverage of the Switch platform by domain. **Generated** by `tools/support/gen_support.py` from the percent-analysis snapshot `2026-11-02` (commit `8389280`, kept in `tools/support/data/coverage-snapshot.json`); do not edit by hand. The score is an estimate of how much of the platform the pipeline and runtime implement, not per-game compatibility: see the [support matrix](support-matrix.md) for titles. An item is `impl` when implemented and checked by a test or a run, `partial` or `stub` when it exists but is incomplete, `missing` when absent.

**Overall: 98.9%** (range 98.9-98.9).

## By domain

| Domain | Weight | Coverage | Points | impl | partial | stub | missing |
|---|--:|--:|--:|--:|--:|--:|--:|
| CPU: AOT A64 rewrite | 14 | 100% | 14.0 | 13 | 0 | 0 | 0 |
| Loader: NSO/NRO/modules/relocs | 5 | 100% | 5.0 | 5 | 0 | 0 | 0 |
| Kernel: Horizon svcs + scheduling | 10 | 100% | 10.0 | 64 | 0 | 0 | 0 |
| IPC: HLE services (coverage) | 10 | 100% | 10.0 | 53 | 0 | 0 | 0 |
| IPC: command depth (impl vs stub) | 5 | 91% | 4.5 | 0 | 0 | 0 | 0 |
| GPU: NVN API surface | 12 | 99% | 11.9 | 518 | 17 | 2 | 0 |
| GPU: Maxwell shader ISA | 12 | 100% | 12.0 | 151 | 0 | 0 | 0 |
| GPU: formats + pipeline features | 8 | 98% | 7.8 | 15 | 2 | 0 | 0 |
| GPU: nvdrv / host1x devices | 2 | 89% | 1.8 | 2 | 3 | 0 | 0 |
| Audio: audren/audout/opus | 7 | 100% | 7.0 | 16 | 0 | 0 | 0 |
| Input: hid / npad | 5 | 99% | 5.0 | 10 | 1 | 0 | 0 |
| Content: containers, crypto, updates, DLC | 5 | 99% | 5.0 | 13 | 1 | 0 | 0 |
| FS: saves + storage | 2 | 100% | 2.0 | 7 | 0 | 0 | 0 |
| Library applets | 2 | 99% | 2.0 | 7 | 0 | 0 | 0 |
| Video / media decode | 1 | 97% | 1.0 | 3 | 1 | 0 | 0 |

Domains with no item list (for example IPC depth) are scored from their own counts in the snapshot; the item columns show 0.

## What is not fully implemented

### GPU: NVN API surface

| Item | Status |
|---|---|
| nvnCommandBufferBindImagesDeferred | partial |
| nvnCommandBufferBindSeparateSampler | partial |
| nvnCommandBufferBindSeparateSamplers | partial |
| nvnCommandBufferBindSeparateSamplersDeferred | partial |
| nvnCommandBufferBindSeparateTexture | partial |
| nvnCommandBufferBindSeparateTextures | partial |
| nvnCommandBufferBindSeparateTexturesDeferred | partial |
| nvnCommandBufferBindStorageBuffersDeferred | partial |
| nvnCommandBufferBindTexturesDeferred | partial |
| nvnCommandBufferBindUniformBuffersDeferred | partial |
| nvnCommandBufferSetConservativeRasterDilate | partial |
| nvnCommandBufferSetConservativeRasterEnable | partial |
| nvnCommandBufferSetProvokingVertexMode | partial |
| nvnCommandBufferSetViewportSwizzles | partial |
| nvnDeviceWaitForError | partial |
| nvnProgramGetSubtileSize | partial |
| nvnTextureBuilderGetViewOffset | partial |
| nvnCommandBufferSetProgramSubroutines | stub |
| nvnProgramSetSubroutineLinkage | stub |

### GPU: formats + pipeline features

| Item | Status |
|---|---|
| tessellation | partial |
| transform feedback | partial |

### GPU: nvdrv / host1x devices

| Item | Status |
|---|---|
| /dev/nvdisp-disp0 | partial |
| /dev/nvhost-gpu, as-gpu, ctrl-gpu (only if NVN is bypassed) | partial |
| /dev/nvhost-nvdec, vic, nvjpg, msenc | partial |

### Input: hid / npad

| Item | Status |
|---|---|
| IR camera (irs) | partial |

### Content: containers, crypto, updates, DLC

| Item | Status |
|---|---|
| NCA2 / NCA0 | partial |

### Video / media decode

| Item | Status |
|---|---|
| JPEG (nvjpg / caps) | partial |

## Next to implement (ranked by coverage gain per effort)

| # | Domain | Item | Effort | Gain (pts) | Why | Where |
|--:|---|---|:-:|--:|---|---|
| 1 | cpu_rewrite | A32 interpreter lda/stl ordering + alignment fault | S | None | debug-only interpreter uses fences around plain accesses (not RCsc) and no alignment fault; the shipped runtime has no interpreter; difftest skips those samples | `runtime/src/a32/a32_exec.c, runtime/tests/a32jit/difftest.c` |
| 2 | nvn | nvnCommandBufferSetProvokingVertexMode | S | 0.01 | 0=first/1=last mapping is a hypothesis; if NVN's default were LAST every title's flat inputs would differ; indirect/GS/tess draws keep first | `gfx/src/mtl_replay.m OP_PROVOKING, api_cmd.c; evidence: title NVN driver method writes` |
| 3 | nvn | nvnCommandBufferSetViewportSwizzles | S | 0.01 | per-viewport swizzles: viewports 1-15 take viewport 0's (single-pass layered/cube rendering uses several) | `gfx/src/shader/maxwell_translate.c vertex epilogue, mtl_replay.m vp_swizzle_key` |
| 4 | nvn | nvnCommandBufferBindSeparateTexture | S | 0.01 | c2 table offsets 0x120/0x520 are a hypothesis (6 BindSeparate* items); confirm from a GLSLC program or the title NVN driver | `gfx/src/shader/maxwell.h SEP_*_C2_BASE` |
| 5 | nvn | nvnCommandBufferBindTexturesDeferred | S | 0.01 | 7 entry points with inferred signatures never called by a title; a driver disassembly would fix them | `gfx/src/nvnapi/nvn_protos_ext.h` |
| 6 | ipc_depth | nifm IRequest Cancel + NetConnect output (offline network-error flow) | S | None | every title's online entry point goes through nn::nifm::HandleNetworkRequestResult; offline it must land once on the console's NetConnect / error UI and return; today Cancel wipes the rejection (no applet), and with that fixed the applet output makes the SDK retry 20x | `runtime/src/services/offline_core.c:120-127, offline.c GetAppletInfo, applet_lib_core.c:341-351; evidence: sdk NSO nn::nifm::HandleNetworkRequestResult, firmware 22.1.0 netConnect applet 0100000000001006` |
| 7 | gpu_fixed | transform feedback | M | 0.1 | XFB layouts self-authored; needs a title or real GLSLC program with XFB (none in the material) | `gfx/src/mtl_stages.inc, shader/control.c` |
| 8 | gpu_fixed | tessellation | M | 0.1 | tess parameters self-authored; needs a real tess program (none in the material) | `gfx/src/mtl_stages.inc` |
| 9 | ipc_depth | local documented-default stubs (fs QueryEntry / GetFileSystemAttribute / GetProgramId / GetRightsId, vi ConvertScalingMode) | S | None | a handful of local rows have documented behaviour; the rest are correct no-ops (no game card, no transfer, one application per runtime) | `runtime/src/services/fs.c:894-2190, vi.c:355` |
| 10 | content | NCA2 / NCA0 | S | 0.04 | pre-release container versions; all retail NCAs in the material are NCA3 | `crates/napple-core nca.rs` |
| 11 | nvn | nvnCommandBufferSetProgramSubroutines | M | 0.01 | subroutine uniforms and linkage dropped (with nvnProgramSetSubroutineLinkage); no title in the material uses them | `gfx/src/api_cmd.c, api_program.c` |
| 12 | nvdrv | /dev/nvhost-nvdec, vic, nvjpg, msenc | S | 0.1 | NVJPG/MSENC/VP9-on-NVDEC tested only; no title opens them | `runtime/src/services/nvdrv_media_core.c` |
| 13 | nvdrv | /dev/nvhost-gpu, as-gpu, ctrl-gpu (only if NVN is bypassed) | S | 0.05 | proposed layouts; need hardware traces | `runtime/src/services/nvdrv_nodes_core.c` |
| 14 | nvdrv | /dev/nvdisp-disp0 | S | 0.05 | unit-tested only; no title opens it | `nvdrv_media_core.c` |
| 15 | input | IR camera (irs) | M | 0.05 | processor layouts need a real Joy-Con IR trace (unreachable on the Mac) | `runtime/src/services/irs.c` |
| 16 | nvn | nvnCommandBufferSetConservativeRasterEnable | L | 0.01 | Metal on Apple GPUs has no conservative rasterization (platform limit) | `gfx/src/api_cmd.c:829` |
| 17 | media | JPEG (nvjpg / caps) | S | 0.03 | NVJPG register path synthetic only | `runtime/src/services/jpeg_core.c` |
| 18 | ipc_depth | offline network service stubs (friend, nifm, ssl, bsd, nsd, npns, bcat, ldn) | L | None | ~200 documented offline defaults; real host networking is a design decision (Nintendo services and LDN peers unreachable), not a gap to close blindly | `runtime/src/services/offline.c, sys_default.h` |
