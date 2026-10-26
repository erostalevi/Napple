# Change archive

Dated notes written by the sessions that built Napple (2026-10-20 to 2026-10-26). They are the history of how and why each part came to be, kept unchanged.
Notes can be out of date: for current behaviour read [architecture](../../architecture/overview.md), [guides](../../guides/README.md) and
[reference](../../reference/subsystems.md); facts that were still true have been folded into those pages. Links inside a note point at the paths of the time
and are not checked.

New change notes go in this folder as `<date>-<branch>.md` and get a line in this index.

Status words in the notes: *verified* (ran on a Mac), *tested* (unit test passed), *unbuilt* (never compiled on macOS), *untested*, *proposed*.

## Build pipeline (Rust: ingest, rewrite, package, signatures)
| Note | Summary |
|---|---|
| [2026-10-20-ingest](2026-10-20-ingest.md) | Milestone 1: XCI/NSP containers, keys, NCA3, ExeFS and RomFS reading in `napple-core`. |
| [2026-10-20-decomp-plan](2026-10-20-decomp-plan.md) | `napple plan`/`decomp`: base, update and DLC selection, NSO loader, rewrite-site scanner. |
| [2026-10-20-signatures](2026-10-20-signatures.md) | Shared signatures: schema, stores, matcher, resolver, REA adapter, opt-in share flow. |
| [2026-10-20-compat-matrix](2026-10-20-compat-matrix.md) | Per-game, per-stage compatibility matrix (`napple-compat`) and NPDM/NACP parsers. |
| [2026-10-21-rewriter](2026-10-21-rewriter.md) | Generic A64 NSO rewriter: x18, TLS registers and svc/counters through a host gate. |
| [2026-10-22-packaging](2026-10-22-packaging.md) | `napple build`: one-process pipeline to a locally signed `.app`, no Python. |
| [2026-10-24-claude-nsz-zstd-2026-10-24-plan11](2026-10-24-claude-nsz-zstd-2026-10-24-plan11.md) | NSZ/XCZ ingest (zstd NCZ streaming and block forms). |
| [2026-10-24-claude-sysdata-firmware-2026-10-24-plan10](2026-10-24-claude-sysdata-firmware-2026-10-24-plan10.md) | Firmware data archives written to `sysdata/` and served by `fsp-srv`. |
| [2026-10-26-claude-nx-rewriter-2026-10-26-nx](2026-10-26-claude-nx-rewriter-2026-10-26-nx.md) | Rewriter correctness and size fixes from the nxnative review (jump tables and others). |
| [2026-10-26-claude-mk8d-64-2026-10-25-titles](2026-10-26-claude-mk8d-64-2026-10-25-titles.md) | MK8D update and DLC on the 64-bit path, in-place RomFS (`romfs.map`), A32 start-up faults. |

## Runtime core, kernel and services
| Note | Summary |
|---|---|
| [2026-10-21-runtime-core](2026-10-21-runtime-core.md) | Game-agnostic native runtime: loader, Horizon kernel surface, IPC framework. |
| [2026-10-22-integration](2026-10-22-integration.md) | Runtime, gfx, audio, input and frontend linked together; first rendered frames. |
| [2026-10-23-input-audio-runtime-round2](2026-10-23-input-audio-runtime-round2.md) | Input remapping, transactional saves, frame pacing, settings overlay, packaged `.app` checks. |
| [2026-10-24-loader-kernel](2026-10-24-loader-kernel.md) | Guest IPC, code memory, NRO loading, host1x channels, save data management (Linux-written). |
| [2026-10-24-services-unbuilt](2026-10-24-services-unbuilt.md) | 24 new HLE services and deeper commands, written unbuilt on Linux. |
| [2026-10-24-services-VERIFY](2026-10-24-services-VERIFY.md) | Verification prompt for the Mac session that builds the services branch. |
| [2026-10-24-claude-services-verify-2026-10-24-plan5](2026-10-24-claude-services-verify-2026-10-24-plan5.md) | Result of that verification: services build and tests on macOS. |
| [2026-10-24-applets-dlc](2026-10-24-applets-dlc.md) | DLC mounting (`aoc:u`) and the error, player-select and controller applets. |
| [2026-10-24-applets-real](2026-10-24-applets-real.md) | Software keyboard config parsing, controller applet answers. |
| [2026-10-24-claude-cloud-small-2026-10-24-plan11](2026-10-24-claude-cloud-small-2026-10-24-plan11.md) | `ncm` as real in-memory tables, `caps`, audren performance metrics. |
| [2026-10-24-claude-tipc-dispatch-2026-10-24-plan8](2026-10-24-claude-tipc-dispatch-2026-10-24-plan8.md) | TIPC request dispatch in `ipc.c`. |
| [2026-10-24-claude-fonts-fallback-2026-10-24-plan7](2026-10-24-claude-fonts-fallback-2026-10-24-plan7.md) | Placeholder shared font when `system/fonts` is missing. |
| [2026-10-24-cleanup-small-plan6](2026-10-24-cleanup-small-plan6.md) | Follow-ups of the services round: result-code constants and verification on a Mac. |
| [2026-10-24-guest-handle](2026-10-24-guest-handle.md) | AC "request on handle 0" abort: sm GetServiceHandle refusal over TIPC. |
| [2026-10-24-boot-crash](2026-10-24-boot-crash.md) | Module-init SIGSEGV that did not reproduce (no source change). |
| [2026-10-26-nx-kernel](2026-10-26-nx-kernel.md) | Kernel, thread, input and service fixes from the nxnative review (same-core handoff and others). |

## Graphics and shaders
| Note | Summary |
|---|---|
| [2026-10-22-graphics](2026-10-22-graphics.md) | `libnapple_gfx` first slice: NVN on Metal, native Maxwell to MSL translator, headless tests. |
| [2026-10-23-graphics-round2](2026-10-23-graphics-round2.md) | LAS white-out root cause (missing vertex format) and shared correctness fixes. |
| [2026-10-23-crash-and-gfx-round3](2026-10-23-crash-and-gfx-round3.md) | LAS crash on leaving Marin's house, graphics backlog, SMO. |
| [2026-10-23-smo-round](2026-10-23-smo-round.md) | SMO black world and deformed characters: command-buffer handle lifetime, crash fixes. |
| [2026-10-24-nvn-pipeline](2026-10-24-nvn-pipeline.md) | Pipeline-build hang, more NVN entry points and pipeline features. |
| [2026-10-24-claude-nvn-getters-2026-10-24-plan5](2026-10-24-claude-nvn-getters-2026-10-24-plan5.md) | NVN getters/setters visible to the coverage scanner, transform feedback and GL-sync entries. |
| [2026-10-24-shader-isa](2026-10-24-shader-isa.md) | Maxwell ISA gaps (LDS/STS, TLD, TXD, atomics, VOTE and more). |
| [2026-10-24-shader-isa-INTEGRATE](2026-10-24-shader-isa-INTEGRATE.md) | How the shader ISA branch was landed. |
| [2026-10-25-nx-shader](2026-10-25-nx-shader.md) | Translator items from the nxnative review (VMNMX and others), SMO glitch triage. |
| [2026-10-26-nx-gfx](2026-10-26-nx-gfx.md) | Animal Crossing window pass investigation; wrapped up early. |

## Audio, input, media
| Note | Summary |
|---|---|
| [2026-10-21-audio-input-frontend](2026-10-21-audio-input-frontend.md) | The audio, input and frontend static libraries and their C headers. |
| [2026-10-24-audio-i3dl2-audin-review](2026-10-24-audio-i3dl2-audin-review.md) | I3DL2 reverb fixes, `audin:u`, audren review. |
| [2026-10-24-audio-small](2026-10-24-audio-small.md) | `audrec:u`, svc 0x23, shader IMAD and LEA; reverb already present. |
| [2026-10-24-claude-media-h264-2026-10-24-plan5](2026-10-24-claude-media-h264-2026-10-24-plan5.md) | VideoToolbox H.264 decoder for the movie library. |
| [2026-10-24-h264-decoder-type](2026-10-24-h264-decoder-type.md) | Decoder type to mime table read from the movie library. |
| [2026-10-24-verify-media-dlc](2026-10-24-verify-media-dlc.md) | Scripted checks of H.264, DLC and a flaky guest Break. |

## Game bring-up (Animal Crossing, Link's Awakening, Odyssey)
| Note | Summary |
|---|---|
| [2026-10-24-ac-boot-spin](2026-10-24-ac-boot-spin.md) | Boot-thread spin on a never-signalled system event fixed, `ngc:u` added. |
| [2026-10-24-ac-games](2026-10-24-ac-games.md) | Animal Crossing past its aborts, delta RomFS, DLC check. |
| [2026-10-24-ac-shaders](2026-10-24-ac-shaders.md) | "Loading shaders" screen is not shader work: the game submits no shader. |
| [2026-10-24-claude-ac-boot-crash-2026-10-24-plan8](2026-10-24-claude-ac-boot-crash-2026-10-24-plan8.md) | AC v0 SIGSEGV from the NVN API version check. |
| [2026-10-24-claude-ac-services-2026-10-24-plan9](2026-10-24-claude-ac-services-2026-10-24-plan9.md) | Service blockers for Animal Crossing (`fsp-srv` 202 and others). |
| [2026-10-25-claude-ac-render-2026-10-25-titles](2026-10-25-claude-ac-render-2026-10-25-titles.md) | AC 3.0.3 main loop and first frames; Npad lifo reader loop. |
| [2026-10-25-perf-las-live](2026-10-25-perf-las-live.md) | LAS slowdowns measured in live play. |

## AArch32 (Mario Kart 8 Deluxe)
| Note | Summary |
|---|---|
| [2026-10-25-aarch32-core](2026-10-25-aarch32-core.md) | The AArch32 CPU core (interpreter): why it is separate, status, plan. |
| [2026-10-25-aarch32-recompiler-plan](2026-10-25-aarch32-recompiler-plan.md) | Measurements on the real game and the recompiler plan. |
| [2026-10-25-mk8d-recompiler](2026-10-25-mk8d-recompiler.md) | Ahead-of-time AArch32 to AArch64 recompiler producing `a32jit.dylib`. |

## Studio and iOS
| Note | Summary |
|---|---|
| [2026-10-20-studio](2026-10-20-studio.md) | Napple Studio (SwiftUI): key sets, drop zone, plan, build, compat banner. |
| [2026-10-22-studio-final](2026-10-22-studio-final.md) | Studio driving the real `napple build`. |
| [2026-10-25-studio-redesign](2026-10-25-studio-redesign.md) | One-window library design. |
| [2026-10-24-ios-support](2026-10-24-ios-support.md) | iOS/iPadOS 26+ target: dylib modules, signing, IPA, install. |
| [2026-10-25-ios-sim-titles](2026-10-25-ios-sim-titles.md) | The four titles on the iOS Simulator. |

## Process, CI, reviews
| Note | Summary |
|---|---|
| [2026-10-23-cloud-session](2026-10-23-cloud-session.md) | First Linux cloud session: Rust workspace off macOS, rewriter fixes, gfx proposals. |
| [2026-10-25-nxnative-review](2026-10-25-nxnative-review.md) | Review of the nxnative project: what Napple can take (review only). |
| [2026-10-26-nx-ci-docs](2026-10-26-nx-ci-docs.md) | CI workflow, symptom index, subsystem reference, title notes, docs check. |
| [2026-10-26-code-slim](2026-10-26-code-slim.md) | Slim batch: one-time clang-format/rustfmt pass, warnings and clippy to zero, dead code, TODO triage into `docs/todo.md`. |
| [2026-10-26-repo-hygiene](2026-10-26-repo-hygiene.md) | Slim batch: LICENSE and community files, README, `tools/dev` and `tools/legacy`, process archive, legacy projects removed. |
| [2026-10-26-docs-consolidate](2026-10-26-docs-consolidate.md) | Slim batch: docs restructured into architecture, guides, reference, process and archive; mkdocs site; docs check extended. |
| [2026-10-26-mac-slim](2026-10-26-mac-slim.md) | Slim batch (Mac): Obj-C and Swift warnings zero, smaller runtime, shader cache cap, `napple clean`, Makefile dependency tracking. |
| [2026-10-26-f64-isa](2026-10-26-f64-isa.md) | Bit-exact double precision (DADD/DMUL/DFMA/DSETP/DMNMX/DSET, 64-bit conversions) for the Maxwell translator and the 40-item rare-ISA audit. |
| [2026-10-26-claude-support-tables-2026-10-26-cloud2](2026-10-26-claude-support-tables-2026-10-26-cloud2.md) | Generated support matrix, feature-support table and site JSON with a `--check` staleness guard. |
| [2026-10-26-claude-media-services-2026-10-26-cloud2](2026-10-26-claude-media-services-2026-10-26-cloud2.md) | nvdrv multimedia engines (NVDEC/VIC/NVJPG/MSENC) over a DecoderBackend, portable JPEG codec, caps album images, Vorbis/Opus movie audio (cloud, Linux-tested). |
| [2026-10-26-runtime-depth](2026-10-26-runtime-depth.md) | Stub IPC rows become real behaviour with host tests: hid, bcat, audio, psm, lbl, settings, apm, pdm, pctl/aoc and part of spsm/ovln/ectx/fgm/olsc/time/ns/ncm/ngc. |
| [2026-10-26-shader-surface](2026-10-26-shader-surface.md) | SULD/SUST/SUATOM/SURED translated to Metal image calls, with the image binding contract. |
