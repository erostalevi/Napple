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
| [2026-10-26-claude-gfx-correctness-2026-10-26-macos1](2026-10-26-claude-gfx-correctness-2026-10-26-macos1.md) | gfx correctness (macOS-100% batch 1): command-list lifetime, block-compressed render targets, D24/integer MSAA, golden-frame harness, ACNH window-pass root cause. |

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
| [2026-10-26-mii-firmware](2026-10-26-mii-firmware.md) | Mii data and services from the firmware (default Miis, shared Mii database, mii:u/mii:e), the real Mii Maker applet booted as a child process or in-process library. |
| [2026-10-26-claude-studio-redo-2026-10-26](2026-10-26-claude-studio-redo-2026-10-26.md) | Napple Studio rebuilt from zero (Liquid Glass), iOS install/IPA export, per-title bundle ids, free/paid memory-limit rule; amiibo and IR camera tabs in the in-game overlay. |
| [2026-10-27-claude-render-scale-2026-10-27](2026-10-27-claude-render-scale-2026-10-27.md) | Render scale 10..800 %, output fit incl. integer scale, built-in upscaler (FSR-like, CAS), MetalFX fallback, budget, per-title rule table, two render-pass fixes found at non-1x scales. |
| [2026-10-27-claude-contributor-kit-2026-10-27](2026-10-27-claude-contributor-kit-2026-10-27.md) | Contributor kit: legal rules and DCO sign-off in CONTRIBUTING, issue forms, PR template, contributor guides, catalogue field reference, labels and Discussions proposal, issue-form check (cloud, Linux-tested). |
| [2026-10-27-claude-youtube-script-2026-10-27](2026-10-27-claude-youtube-script-2026-10-27.md) | YouTube video kit under `docs/media/youtube/`: shooting script, call to action, shot list, titles/thumbnails/description. Docs only. |
| [2026-10-27-mod-framework](2026-10-27-mod-framework.md) | The shareable `.napmod` mod and fix format: IPS/IPS32/pchtxt patches, cheats, romfs overlay, config, shader files, trust model, `napple mods` CLI and the pre-rewrite apply hook (portable core, Linux-tested). |
| [2026-10-27-claude-small-gaps-2026-10-27](2026-10-27-claude-small-gaps-2026-10-27.md) | Nine small catalog items: personalized tickets, IVFC verification, NCA2/NCA0, NCZ edge, audren performance history, audin sources/mic, GC/Palma/NES styles, irs rows, bcat provider (cloud, Linux-tested). |
| [2026-10-27-claude-ipc-depth-rest-2026-10-27](2026-10-27-claude-ipc-depth-rest-2026-10-27.md) | 137 more stub IPC rows become real behaviour with host tests: offline (mm, friend, nifm, nsd, prepo, ssl, nfc), btm, hidbus, ldn, ns, ncm, sfdnsres, ngc, push. |
| [2026-10-27-fps-unlock-2026-10-27](2026-10-27-fps-unlock-2026-10-27.md) | Present rate (fps_mode: game / display / custom / uncapped) decoupled from game speed: guest vsync and game clocks unchanged, vsync setting drives the layer, frame-trace gate. |
| [2026-10-27-claude-media-nvdrv-2026-10-27](2026-10-27-claude-media-nvdrv-2026-10-27.md) | Vorbis out-of-band headers (on by default), nvdisp-disp0/1 query ioctls, nvdrv stub rows 7 -> 1, DecoderBackend ABI 1 frozen with SPS/PPS/vpcC hand-off for a VideoToolbox backend (cloud, Linux-tested). |
- [2026-10-27-tie-up.md](2026-10-27-tie-up.md): tie-up: one system data root, Studio Mii/Amiibo/Online panes, amiibo loader, Mii Maker host, camera/microphone entitlements, `tools/e2e` end-to-end run per title.
| [2026-10-27-claude-content-real-2026-10-27](2026-10-27-claude-content-real-2026-10-27.md) | NSZ/XCZ written by the nsz tool, NCA2/NCA0 after hactool, personalized tickets through `Container::load`: fixtures, round trips, and the XCI header offsets the real writer exposed (cloud, Linux-tested). |
- [2026-10-27-claude-applets-host-2026-10-27.md](2026-10-27-claude-applets-host-2026-10-27.md): generic library applet host: handler registry, firmware applet table, interactive channel, swkbd inline, applet.c + applet_ae.c stub rows 84 -> 10 (tested on Linux).
| [2026-10-27-mod-glue-core](2026-10-27-mod-glue-core.md) | `napple build --mods` (patches before the AOT rewrite, manifest record, mod-set cache key, mods/overlay.json), runtime romfs overlay lookup in romfs_map.c, control.nacp in every data dir (cloud, Linux-tested). |
- [2026-10-27-claude-shader-stages-2026-10-27.md](2026-10-27-claude-shader-stages-2026-10-27.md): translator side of geometry, tessellation and transform feedback stages, OUT/VABSDIFF4/TXA/IMADSP (Metal-compiled on the Mac).
- [2026-10-27-claude-small-catalog-2026-10-27.md](2026-10-27-claude-small-catalog-2026-10-27.md): audren performance, npad style bits, IR tera/pointing processors, bcat layouts.
- [2026-10-27-claude-ipc-depth-rest2-2026-10-27.md](2026-10-27-claude-ipc-depth-rest2-2026-10-27.md): remaining stub IPC rows (account, vi, ns, settings, offline, miiimg) and network rows through OnlineProvider.

- [2026-10-27-claude-applets-rare-2026-10-27.md](2026-10-27-claude-applets-rare-2026-10-27.md): HLE web / offline web and photo viewer / album library applets over the handler registry, applet_ui.h hooks (tested on Linux, unbuilt on macOS).
- [2026-10-27-kernel-threads.md](2026-10-27-kernel-threads.md): kernel-threads: timer preemption of spinning guest threads (SMO stall fixed), LAS save-commit race (0x35f202), post-mortem svc ring, ACNH load-only null deref documented.
- [2026-10-27-gfx-depth-views.md](2026-10-27-gfx-depth-views.md): ResolveDepthBuffer, depth/stencil texture views, SetSampleShading side table, SetDebugLabel, gfx warning fixes.
- [2026-10-27-claude-ios-candidate-2026-10-27.md](2026-10-27-claude-ios-candidate-2026-10-27.md): iOS Simulator profiling, memlog, handheld default on iOS, build_ios fixes (iOS-only code paths; guide `docs/guides/ios-device-candidate.md`).
- [2026-10-27-claude-fuzz-hardening-2026-10-27.md](2026-10-27-claude-fuzz-hardening-2026-10-27.md): seeded-mutation tests and cargo-fuzz / C fuzz drivers for the untrusted-input parsers; 26 findings fixed with regression tests (cloud, Linux-tested).
- [2026-10-27-claude-ipc-polish-2026-10-27.md](2026-10-27-claude-ipc-polish-2026-10-27.md): IPC command depth toward 100% (remaining stub rows, titles' known-call list, check_known_calls on every list).
- [2026-10-27-claude-shader-rare-ops-2026-10-27.md](2026-10-27-claude-shader-rare-ops-2026-10-27.md): the last 18 rare Maxwell ops and stencil texture bindings in the translator (ShaderKey.stencil_slots).
- [2026-10-27-claude-build-content-2026-10-27-b6.md](2026-10-27-claude-build-content-2026-10-27-b6.md): NSZ/XCZ zstd containers and HtmlDocument extraction to `<data-dir>/html` (cloud, tested on Linux).
- [2026-10-27-claude-media-mac-2026-10-27-b6.md](2026-10-27-claude-media-mac-2026-10-27-b6.md): VideoToolbox DecoderBackend, MK8D's NVDEC/VIC stream shape, movie audio and JPEG (Mac; the Grand Prix preview panel still shows no video).
- [2026-10-27-glue-up.md](2026-10-27-glue-up.md): glue-up pass on the Mac: web/album applets and stencil view slots hooked, fs log rows, timescale flake, deterministic a32jit, every suite and the e2e/golden/Studio runs; the bigger-work list for the next round.
- [2026-10-27-claude-runtime-small-2026-10-27-b6.md](2026-10-27-claude-runtime-small-2026-10-27-b6.md): vi producer transactions (TransactParcel rows implemented), npad style helpers (GameCube/Palma/NES in the system policy), tests for the unused core API; audin/audren already covered elsewhere.
- [2026-10-27-claude-applets-frontend-2026-10-27.md](2026-10-27-claude-applets-frontend-2026-10-27.md): AppKit sheets for the controller, error, player select, web (WKWebView) and album applets, scripted headless answers, `make -C frontend test-sheets`.
- [2026-10-27-claude-gfx-features-2026-10-27-b6.md](2026-10-27-claude-gfx-features-2026-10-27-b6.md): replay of transform feedback, geometry shaders (compute emulation) and tessellation (TCS kernel + drawPatches); stencil view sampled in a shader test.
- [2026-10-28-claude-runtime-small-2-2026-10-27-b7.md](2026-10-28-claude-runtime-small-2-2026-10-27-b7.md): test_timescale measured jump bound, audin mic permission hook, Linux ipc fuzz target builds again; the other runtime-small-2 items were already in place (tested, not verified)
| [2026-10-27-claude-mods-runtime-2026-10-27-b7](2026-10-27-claude-mods-runtime-2026-10-27-b7.md) | Mods in the runtime: merged RomFS image for the path-level overlay (fs.c), cheat VM with loops/registers/pointer chains (Rust + C twin) run per frame, mod config applied to the run's settings, `--mods-strict`, stale mods files cleared on rebuild (cloud, Linux-tested). |
- [2026-10-27-claude-media-nvdec-preview-2026-10-27-b7.md](2026-10-27-claude-media-nvdec-preview-2026-10-27-b7.md): MK8D's preview videos play through nvhost-nvdec + VIC (VIC config at 0x1c2, NVDEC slot from curr_pic_idx, 15 reference frames in the synthesized SPS); decoder pitch, real VIC layout, caps:dc DecodeJpeg.
- [2026-10-28-claude-gpu-title-scanner-2026-10-28-b8.md](2026-10-28-claude-gpu-title-scanner-2026-10-28-b8.md): tools/gpu/scan_stages.py (control-section stage scan, RomFS scan, set-cover of titles); tested on synthetic sections, not verified on real titles
- [2026-10-28-claude-gfx-recorder-flake-2026-10-28-b8.md](2026-10-28-claude-gfx-recorder-flake-2026-10-28-b8.md): ACNH start crash: command buffer host parts by slot+generation, locked recording state, NAPPLE_GFX_CBTRACE tracer, t_cmdshare; guest double-job race still open
- [2026-10-28-claude-gfx-stages-complete-2026-10-28-b8.md](2026-10-28-claude-gfx-stages-complete-2026-10-28-b8.md): software tessellator (isolines, TES XFB, GS after tess), gs_run refactor, control-section length fix
- [2026-10-28-claude-nvdrv-media-devices-2026-10-28-b8.md](2026-10-28-claude-nvdrv-media-devices-2026-10-28-b8.md): NVDEC/VIC pitches, VP9, NVJPG encode, MSENC encoder backend (VideoToolbox H.264)
- [2026-10-28-claude-nvdrv-device-nodes-2026-10-28-b8.md](2026-10-28-claude-nvdrv-device-nodes-2026-10-28-b8.md): state-tracked ioctls for nvhost-gpu, nvhost-as-gpu, nvhost-ctrl-gpu
