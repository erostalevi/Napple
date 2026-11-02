# Feature support

Coverage of the Switch platform by domain. **Generated** by `tools/support/gen_support.py` from the percent-analysis snapshot `2026-11-02` (commit `2da952e`, kept in `tools/support/data/coverage-snapshot.json`); do not edit by hand. The score is an estimate of how much of the platform the pipeline and runtime implement, not per-game compatibility: see the [support matrix](support-matrix.md) for titles. An item is `impl` when implemented and checked by a test or a run, `partial` or `stub` when it exists but is incomplete, `missing` when absent.

**Overall: 99.3%** (range 99.3-99.3).

## By domain

| Domain | Weight | Coverage | Points | impl | partial | stub | missing |
|---|--:|--:|--:|--:|--:|--:|--:|
| CPU: AOT A64 rewrite | 14 | 100% | 14.0 | 13 | 0 | 0 | 0 |
| Loader: NSO/NRO/modules/relocs | 5 | 100% | 5.0 | 5 | 0 | 0 | 0 |
| Kernel: Horizon svcs + scheduling | 10 | 100% | 10.0 | 64 | 0 | 0 | 0 |
| IPC: HLE services (coverage) | 10 | 100% | 10.0 | 53 | 0 | 0 | 0 |
| IPC: command depth (impl vs stub) | 5 | 98% | 4.9 | 0 | 0 | 0 | 0 |
| GPU: NVN API surface | 12 | 99% | 11.9 | 518 | 17 | 2 | 0 |
| GPU: Maxwell shader ISA | 12 | 100% | 12.0 | 151 | 0 | 0 | 0 |
| GPU: formats + pipeline features | 8 | 98% | 7.8 | 15 | 2 | 0 | 0 |
| GPU: nvdrv / host1x devices | 2 | 92% | 1.9 | 3 | 2 | 0 | 0 |
| Audio: audren/audout/opus | 7 | 100% | 7.0 | 16 | 0 | 0 | 0 |
| Input: hid / npad | 5 | 99% | 5.0 | 10 | 1 | 0 | 0 |
| Content: containers, crypto, updates, DLC | 5 | 99% | 5.0 | 13 | 1 | 0 | 0 |
| FS: saves + storage | 2 | 100% | 2.0 | 7 | 0 | 0 | 0 |
| Library applets | 2 | 100% | 2.0 | 7 | 0 | 0 | 0 |
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
| 1 | cpu_rewrite | A32 interpreter lda/stl ordering + alignment fault | S | None | debug-only interpreter; shipped runtime has none | `runtime/src/a32/a32_exec.c` |
| 2 | nvn | nvnCommandBufferSetProvokingVertexMode | S | 0.01 | 0=first/1=last mapping is a hypothesis; indirect/GS/tess draws keep first | `gfx/src/mtl_replay.m OP_PROVOKING` |
| 3 | nvn | nvnCommandBufferBindSeparateTexture | S | 0.01 | c2 table offsets 0x120/0x520 are a hypothesis (6 BindSeparate* items); confirm from a GLSLC program | `gfx/src/shader/maxwell.h SEP_*_C2_BASE` |
| 4 | nvn | nvnCommandBufferSetViewportSwizzles | S | 0.01 | viewports 1-15 take viewport 0's swizzle | `gfx/src/shader/maxwell_translate.c, mtl_replay.m` |
| 5 | nvn | nvnCommandBufferBindTexturesDeferred | S | 0.01 | 7 entry points with inferred signatures; a driver disassembly fixes them | `gfx/src/nvnapi/nvn_protos_ext.h` |
| 6 | ipc_depth | IPC risk rows untested on a title (applet ExecuteProgram execv, fs permission-bit checks, 0x1907C for unlinked users) | S | None | landed with host/hook tests only; a title that uses them would exercise them | `runtime/src/services/applet.c, fs.c, account.c` |
| 7 | gpu_fixed | transform feedback | M | 0.1 | XFB layouts self-authored; needs a title or real GLSLC program with XFB | `gfx/src/mtl_stages.inc, shader/control.c` |
| 8 | gpu_fixed | tessellation | M | 0.1 | tess parameters self-authored; needs a real tess program | `gfx/src/mtl_stages.inc` |
| 9 | content | NCA2 / NCA0 | S | 0.04 | pre-release containers; no retail sample | `crates/napple-core nca.rs` |
| 10 | nvdrv | /dev/nvhost-nvdec, vic, nvjpg, msenc | S | 0.1 | VIC 4.0 struct trace-matched; NVJPG header-less scan refused; no title drives NVJPG/MSENC | `runtime/src/services/nvdrv_media_core.c` |
| 11 | nvn | nvnCommandBufferSetProgramSubroutines | M | 0.01 | subroutine uniforms/linkage dropped; no title uses them | `gfx/src/api_cmd.c, api_program.c` |
| 12 | ipc_depth | remaining IPC_STUB rows (35 reference rows: ISslConnection, IApplicationFunctions, IGeneralService, bsd:u, ILibraryAppletAccessor, ISelfController) | M | None | need OnlineProvider hooks; each row has its reason in the table comment | `runtime/src/services/offline.c, push.c, bcat.c` |
| 13 | ipc_depth | LDN join path crash after joining a Napple room (station dies right after join) | M | None | local wireless is a common feature class; join must not take the guest down | `runtime/src/services/ldn.c, ldn_core.c` |
| 14 | ipc_depth | friend:u list stays empty in a title although the Napple profile has friends | M | None | friend list/presence is the common online-feature path | `runtime/src/services/offline.c, online_provider.h` |
| 15 | nvdrv | /dev/nvhost-gpu, as-gpu, ctrl-gpu (only if NVN is bypassed) | S | 0.05 | apply the one-line nvdrv.c routing to nvgpu_chan_ioctl_typed (nvdrv.c:643); rest needs a title that bypasses NVN | `runtime/src/services/nvdrv.c, nvdrv_nodes_core.c` |
| 16 | input | IR camera (irs) | M | 0.05 | processor layouts need a Joy-Con R IR trace (hardware) | `runtime/src/services/irs.c` |
| 17 | nvn | nvnCommandBufferSetConservativeRasterEnable | L | 0.01 | Metal on Apple GPUs has no conservative rasterization (platform limit) | `gfx/src/api_cmd.c:829` |
| 18 | media | JPEG (nvjpg / caps) | S | 0.03 | NVJPG register path not exercised by a title | `runtime/src/services/jpeg_core.c` |
| 19 | ipc_depth | offline network service stubs (friend, nifm, ssl, bsd, nsd, npns, bcat, ldn) | L | None | by design: Nintendo services unreachable; Napple-run online replaces them title by title | `runtime/src/services/offline.c, runtime/src/net` |
