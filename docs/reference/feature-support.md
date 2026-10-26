# Feature support

Coverage of the Switch platform by domain. **Generated** by `tools/support/gen_support.py` from the percent-analysis snapshot `2026-10-26` (commit `332988e`, kept in `tools/support/data/coverage-snapshot.json`); do not edit by hand. The score is an estimate of how much of the platform the pipeline and runtime implement, not per-game compatibility: see the [support matrix](support-matrix.md) for titles. An item is `impl` when implemented and checked by a test or a run, `partial` or `stub` when it exists but is incomplete, `missing` when absent.

**Overall: 91.5%** (range 91.5-91.5).

## By domain

| Domain | Weight | Coverage | Points | impl | partial | stub | missing |
|---|--:|--:|--:|--:|--:|--:|--:|
| CPU: AOT A64 rewrite | 14 | 100% | 14.0 | 13 | 0 | 0 | 0 |
| Loader: NSO/NRO/modules/relocs | 5 | 100% | 5.0 | 5 | 0 | 0 | 0 |
| Kernel: Horizon svcs + scheduling | 10 | 100% | 10.0 | 64 | 0 | 0 | 0 |
| IPC: HLE services (coverage) | 10 | 100% | 10.0 | 53 | 0 | 0 | 0 |
| IPC: command depth (impl vs stub) | 5 | 52% | 2.6 | 0 | 0 | 0 | 0 |
| GPU: NVN API surface | 12 | 100% | 12.0 | 523 | 0 | 0 | 0 |
| GPU: Maxwell shader ISA | 12 | 83% | 9.9 | 102 | 0 | 0 | 49 |
| GPU: formats + pipeline features | 8 | 79% | 6.3 | 11 | 3 | 0 | 3 |
| GPU: nvdrv / host1x devices | 2 | 80% | 1.6 | 2 | 0 | 3 | 0 |
| Audio: audren/audout/opus | 7 | 98% | 6.8 | 14 | 2 | 0 | 0 |
| Input: hid / npad | 5 | 97% | 4.8 | 8 | 3 | 0 | 0 |
| Content: containers, crypto, updates, DLC | 5 | 90% | 4.5 | 10 | 1 | 0 | 3 |
| FS: saves + storage | 2 | 98% | 2.0 | 6 | 1 | 0 | 0 |
| Library applets | 2 | 58% | 1.1 | 0 | 4 | 0 | 3 |
| Video / media decode | 1 | 81% | 0.8 | 1 | 2 | 1 | 0 |

Domains with no item list (for example IPC depth) are scored from their own counts in the snapshot; the item columns show 0.

## What is not fully implemented

### GPU: Maxwell shader ISA

| Item | Status |
|---|---|
| B2R | missing |
| BPT | missing |
| CAL | missing |
| CCTL | missing |
| CCTLL | missing |
| DADD | missing |
| DFMA | missing |
| DMNMX | missing |
| DMUL | missing |
| DSET | missing |
| DSETP | missing |
| FCHK | missing |
| GETCRSPTR | missing |
| GETLMEMBASE | missing |
| IDE | missing |
| IDP | missing |
| IMAD32I | missing |
| IMADSP | missing |
| ISBERD | missing |
| JCAL | missing |
| JMP | missing |
| JMX | missing |
| LEPC | missing |
| LONGJMP | missing |
| OUT | missing |
| PEXIT | missing |
| PIXLD | missing |
| PLONGJMP | missing |
| R2B | missing |
| RAM | missing |
| RET | missing |
| RTT | missing |
| SAM | missing |
| SETCRSPTR | missing |
| SETLMEMBASE | missing |
| STP | missing |
| SUATOM | missing |
| SULD | missing |
| SURED | missing |
| SUST | missing |
| TXA | missing |
| VABSDIFF | missing |
| VABSDIFF4 | missing |
| VADD | missing |
| VMAD | missing |
| VSET | missing |
| VSETP | missing |
| VSHL | missing |
| VSHR | missing |

### GPU: formats + pipeline features

| Item | Status |
|---|---|
| MSAA / resolve | partial |
| depth/stencil formats | partial |
| texture views / sparse | partial |
| geometry shaders | missing |
| tessellation | missing |
| transform feedback | missing |

### GPU: nvdrv / host1x devices

| Item | Status |
|---|---|
| /dev/nvdisp-disp0 | stub |
| /dev/nvhost-gpu, as-gpu, ctrl-gpu (only if NVN is bypassed) | stub |
| /dev/nvhost-nvdec, vic, nvjpg, msenc | stub |

### Audio: audren/audout/opus

| Item | Status |
|---|---|
| audin (microphone) | partial |
| performance metrics | partial |

### Input: hid / npad

| Item | Status |
|---|---|
| GameCube / Palma / NES styles | partial |
| IR camera (irs) | partial |
| gesture / debugpad | partial |

### Content: containers, crypto, updates, DLC

| Item | Status |
|---|---|
| NSZ / XCZ (zstd) | partial |
| IVFC hash-tree verification | missing |
| NCA2 / NCA0 | missing |
| personalized tickets | missing |

### FS: saves + storage

| Item | Status |
|---|---|
| bcat storage | partial |

### Library applets

| Item | Status |
|---|---|
| controller applet | partial |
| error applet | partial |
| player select (profile) | partial |
| swkbd (software keyboard) | partial |
| mii edit | missing |
| photo viewer / album | missing |
| web / offline web | missing |

### Video / media decode

| Item | Status |
|---|---|
| H.264 | partial |
| Vorbis/Opus movie audio | partial |
| JPEG (nvjpg / caps) | stub |

## Next to implement (ranked by coverage gain per effort)

| # | Domain | Item | Effort | Gain (pts) | Why | Where |
|--:|---|---|:-:|--:|---|---|
| 1 | audio | performance metrics | S | 0.1 | modelled buffer, layout unverified against a title | `audio/src/audren.c` |
| 2 | content | NSZ / XCZ (zstd) | S | 0.07 | implemented, needs a real NSZ/XCZ dump to verify | `crates/napple-core ncz.rs` |
| 3 | gpu_fixed | depth/stencil formats | M | 0.14 | D24/D24S8 substituted by D32F, ResolveDepthBuffer no-op; every 3D title | `gfx/src/tex/napple_fmt.c, gfx/src/api_cmd.c` |
| 4 | applets | swkbd (software keyboard) | S | 0.12 | text entry applet; unit-tested only (ACNH name entry reached by script, not via swkbd) | `runtime/src/services/applet.c, app.m` |
| 5 | gpu_fixed | MSAA / resolve | M | 0.1 | integer resolves skipped, ResolveDepthBuffer no-op | `gfx/src/mtl_replay.m, api_cmd.c` |
| 6 | input | gesture / debugpad | S | 0.05 | activate succeeds, data never produced | `runtime/src/services/hid.c` |
| 7 | applets | error applet | S | 0.08 | no real UI, unit-tested only | `runtime/src/services/applet.c` |
| 8 | applets | player select (profile) | S | 0.08 | single profile, no cancel path | `runtime/src/services/applet.c` |
| 9 | media | H.264 | S | 0.08 | VideoToolbox path exists, never driven by a guest; decoder type guess | `runtime/src/movie.c, vt_h264.c` |
| 10 | shader | SULD | L | 0.07 | image loads need an image binding contract | `gfx/src/shader, mtl_shader.m` |
| 11 | shader | SUST | L | 0.07 | image stores, same contract | `gfx/src/shader, mtl_shader.m` |
| 12 | gpu_fixed | texture views / sparse | M | 0.05 | views Metal cannot express fall back to the whole texture | `gfx/src/mtl_tex.m` |
| 13 | applets | controller applet | M | 0.12 | always confirms one pad; no pairing UI for multi-player | `runtime/src/services/applet.c` |
| 14 | gpu_fixed | transform feedback | L | 0.38 | no record/replay path; Metal vertex-capture emulation | `gfx/src/api_cmd.c, mtl_replay.m` |
| 15 | gpu_fixed | tessellation | L | 0.38 | patch size/levels are no-ops, no tess stages | `gfx/src/shader, api_cmd.c` |
| 16 | gpu_fixed | geometry shaders | L | 0.38 | no geometry stage in Metal; compute emulation | `gfx/src/shader` |
| 17 | media | JPEG (nvjpg / caps) | S | 0.06 | ImageIO helpers not wired to caps/nvdrv | `runtime/src/services/caps.c` |
| 18 | media | Vorbis/Opus movie audio | M | 0.06 | movie audio types Vorbis/AAC are not hooked | `runtime/src/movie.c` |
| 19 | applets | mii edit | M | 0.15 | mii editor applet | `runtime/src/services/applet.c` |
| 20 | nvdrv | /dev/nvhost-nvdec, vic, nvjpg, msenc | L | 0.2 | ioctl surface only, no decode engines | `runtime/src/services/nvdrv.c` |
| 21 | applets | web / offline web | L | 0.15 | web applet | `runtime/src/services/applet.c` |
