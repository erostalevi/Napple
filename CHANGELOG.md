# Changelog

All notable changes to Napple are recorded here, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Napple has no hand-curated releases yet. Every commit to `main` publishes a release named `v0.1.<run number>` (see `.github/workflows/release.yml`; the
`0.1` part is `MAJOR_MINOR` in that file), so a version number identifies a build, not a feature set. Until a stable version scheme exists, entries are grouped
by the date of the change note that describes them. Each entry is the title of a note in [`docs/archive/changes/`](docs/archive/changes/), which holds the details,
what was verified and what was not. Newest first.

## [Unreleased]

- Repository hygiene: license, contribution and security documents, issue and pull request templates, repository layout (see
  [`docs/archive/changes/2026-10-26-repo-hygiene.md`](docs/archive/changes/2026-10-26-repo-hygiene.md)).

## 2026-10-26

- claude/mk8d-64-2026-10-25-titles (2026-10-26): MK8D update + DLC on the 64-bit path, in-place RomFS, A32 base start-up faults ([note](docs/archive/changes/2026-10-26-claude-mk8d-64-2026-10-25-titles.md))
- 2026-10-26 nx-rewriter: rewriter correctness and size picks from the nxnative review (R1, R2, R4, R6) ([note](docs/archive/changes/2026-10-26-claude-nx-rewriter-2026-10-26-nx.md))
- nx-ci-docs (2026-10-26): CI workflow, symptom index, reference, title notes, GPU regression tests ([note](docs/archive/changes/2026-10-26-nx-ci-docs.md))
- nx-gfx (2026-10-26): Animal Crossing window pass investigation (stopgap kept); wrap-up cut short ([note](docs/archive/changes/2026-10-26-nx-gfx.md))
- 2026-10-26 nx-kernel: kernel / thread / input / service rows from the nxnative review ([note](docs/archive/changes/2026-10-26-nx-kernel.md))

## 2026-10-25

- AArch32 (32-bit titles, MK8D): CPU core, status and plan ([note](docs/archive/changes/2026-10-25-aarch32-core.md))
- AArch32 (MK8D): measurements and recompiler plan (Mac session, branch claude/aarch32-recompiler-2026-10-25, based on claude/exciting-davinci-unv0aa 74c7d30) ([note](docs/archive/changes/2026-10-25-aarch32-recompiler-plan.md))
- ac-render (2026-10-25): Animal Crossing 3.0.3 now runs its main loop and draws (frames > 0); AC v0 crash fixed ([note](docs/archive/changes/2026-10-25-claude-ac-render-2026-10-25-titles.md))
- 2026-10-25 ios-sim-titles: the four titles on the iOS Simulator ([note](docs/archive/changes/2026-10-25-ios-sim-titles.md))
- MK8D ahead-of-time AArch32 -> AArch64 recompiler (branch claude/mk8d-recompiler-2026-10-25-titles, Mac) ([note](docs/archive/changes/2026-10-25-mk8d-recompiler.md))
- nx-shader (2026-10-25): translator items from the nxnative review (G7, G13, G14, VMNMX, G11), SMO glitch triage ([note](docs/archive/changes/2026-10-25-nx-shader.md))
- nxnative review: what Napple can take (review only, 2026-10-25) ([note](docs/archive/changes/2026-10-25-nxnative-review.md))
- perf-las-live (2026-10-25): LAS slowdowns measured in live play ([note](docs/archive/changes/2026-10-25-perf-las-live.md))
- Studio redesign: one window, a library of built games ([note](docs/archive/changes/2026-10-25-studio-redesign.md))

## 2026-10-24

- ac-boot-spin: Animal Crossing 3.0.3 boot-thread spin fixed, ngc:u added, honest "no frame yet" text (2026-10-24) ([note](docs/archive/changes/2026-10-24-ac-boot-spin.md))
- ac-games (plan11): Animal Crossing past its aborts; delta romfs; DLC check ([note](docs/archive/changes/2026-10-24-ac-games.md))
- ac-shaders: Animal Crossing 3.0.3 "loading shaders" investigation + shader progress output (2026-10-24) ([note](docs/archive/changes/2026-10-24-ac-shaders.md))
- applets-dlc (batch 2026-10-24-plan5): DLC mounting plus error, player select and controller applets ([note](docs/archive/changes/2026-10-24-applets-dlc.md))
- applets-real (2026-10-24, plan11) ([note](docs/archive/changes/2026-10-24-applets-real.md))
- Audio: I3DL2 reverb fixes, audin, audren review (2026-10-24) ([note](docs/archive/changes/2026-10-24-audio-i3dl2-audin-review.md))
- audio-small (batch 2026-10-24-plan5): audrec:u, svc 0x23, shader IMAD and LEA; I3DL2 already present ([note](docs/archive/changes/2026-10-24-audio-small.md))
- boot-crash (plan6): module-init SIGSEGV did not reproduce on Mac ([note](docs/archive/changes/2026-10-24-boot-crash.md))
- ac-boot-crash (plan8): Animal Crossing v0 SIGSEGV at main+0x19a6250 explained and fixed ([note](docs/archive/changes/2026-10-24-claude-ac-boot-crash-2026-10-24-plan8.md))
- ac-services (plan9): Animal Crossing service blockers ([note](docs/archive/changes/2026-10-24-claude-ac-services-2026-10-24-plan9.md))
- cloud-small: ncm, caps and audren performance metrics (2026-10-24) ([note](docs/archive/changes/2026-10-24-claude-cloud-small-2026-10-24-plan11.md))
- pl:u placeholder font when system/fonts is missing (2026-10-24) ([note](docs/archive/changes/2026-10-24-claude-fonts-fallback-2026-10-24-plan7.md))
- media-h264 (2026-10-24, batch plan5) ([note](docs/archive/changes/2026-10-24-claude-media-h264-2026-10-24-plan5.md))
- NSZ/XCZ ingest (zstd NCZ) ([note](docs/archive/changes/2026-10-24-claude-nsz-zstd-2026-10-24-plan11.md))
- 2026-10-24 NVN getters/setters: scanner-visible definitions, transform-feedback and GL-sync entries ([note](docs/archive/changes/2026-10-24-claude-nvn-getters-2026-10-24-plan5.md))
- services-verify (plan5): Mac build and test of the Linux-written services ([note](docs/archive/changes/2026-10-24-claude-services-verify-2026-10-24-plan5.md))
- sysdata-firmware (plan10) ([note](docs/archive/changes/2026-10-24-claude-sysdata-firmware-2026-10-24-plan10.md))
- TIPC dispatch (runtime/src/ipc.c) ([note](docs/archive/changes/2026-10-24-claude-tipc-dispatch-2026-10-24-plan8.md))
- Cleanup of plan5 follow-ups (branch claude/cleanup-small-2026-10-24-plan6) ([note](docs/archive/changes/2026-10-24-cleanup-small-plan6.md))
- 2026-10-24 guest-handle: AC 3.0.x "request on handle 0" abort (branch claude/guest-handle-2026-10-24-plan10) ([note](docs/archive/changes/2026-10-24-guest-handle.md))
- h264-decoder-type (2026-10-24, batch plan8) ([note](docs/archive/changes/2026-10-24-h264-decoder-type.md))
- iOS / iPadOS 26+ support (2026-10-24, branch claude/wonderful-goldberg-k4752w) ([note](docs/archive/changes/2026-10-24-ios-support.md))
- Loader / kernel round: guest IPC, code memory, NRO loading, host1x channels, save data management ([note](docs/archive/changes/2026-10-24-loader-kernel.md))
- NVN pipeline round: pipeline-build hang, NVN entry points, pipeline features (branch nvn-pipeline-2026-10-24) ([note](docs/archive/changes/2026-10-24-nvn-pipeline.md))
- Verification prompt: services-2026-10-24 (run this on the Mac) ([note](docs/archive/changes/2026-10-24-services-VERIFY.md))
- Services: 24 new services and deeper commands (UNBUILT on macOS) ([note](docs/archive/changes/2026-10-24-services-unbuilt.md))
- Integration notes: shader ISA round (`claude/adoring-dirac-ng6iif`) ([note](docs/archive/changes/2026-10-24-shader-isa-INTEGRATE.md))
- Shader ISA gaps (branch shader-isa) ([note](docs/archive/changes/2026-10-24-shader-isa.md))
- verify-media-dlc (plan7): scripted checks for H.264, DLC and the flaky guest Break ([note](docs/archive/changes/2026-10-24-verify-media-dlc.md))

## 2026-10-23

- Cloud session: Rust workspace off macOS, rewriter fixes, gfx proposals ([note](docs/archive/changes/2026-10-23-cloud-session.md))
- Round 3: LAS crash on leaving Marin's house, graphics backlog, SMO ([note](docs/archive/changes/2026-10-23-crash-and-gfx-round3.md))
- Graphics round 2: LAS in-game white-out root cause (partial round, stopped at a clean point) ([note](docs/archive/changes/2026-10-23-graphics-round2.md))
- Input, audio, saves, services, frame pacing: round 2 (2026-10-23) ([note](docs/archive/changes/2026-10-23-input-audio-runtime-round2.md))
- SMO round: black world, deformed characters, crash fixes ([note](docs/archive/changes/2026-10-23-smo-round.md))

## 2026-10-22

- Graphics: libnapple_gfx (NVN on Metal) first working slice ([note](docs/archive/changes/2026-10-22-graphics.md))
- Integration: runtime + gfx + audio + input + frontend (2026-10-22) ([note](docs/archive/changes/2026-10-22-integration.md))
- Packaging: `napple build` (2026-10-22) ([note](docs/archive/changes/2026-10-22-packaging.md))
- Studio final: real `napple build` integration ([note](docs/archive/changes/2026-10-22-studio-final.md))

## 2026-10-21

- Change: audio, input and frontend libraries ([note](docs/archive/changes/2026-10-21-audio-input-frontend.md))
- Change: generic NSO rewriter (crates/napple-rewrite) ([note](docs/archive/changes/2026-10-21-rewriter.md))
- Runtime core: game-agnostic native runtime (2026-10-21) ([note](docs/archive/changes/2026-10-21-runtime-core.md))

## 2026-10-20

- Compatibility matrix (napple-compat) and NPDM/NACP parsers ([note](docs/archive/changes/2026-10-20-compat-matrix.md))
- Decomp stage: build plan (base/update/DLC), NSO loader, rewrite-site scanner ([note](docs/archive/changes/2026-10-20-decomp-plan.md))
- Milestone 1: ingest (containers, keys, NCA, ExeFS, RomFS) ([note](docs/archive/changes/2026-10-20-ingest.md))
- Shared signatures: schema, stores, matcher, resolver, REA adapter, share flow ([note](docs/archive/changes/2026-10-20-signatures.md))
- Napple Studio (SwiftUI) ([note](docs/archive/changes/2026-10-20-studio.md))
