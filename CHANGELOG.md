# Changelog

All notable changes to Napple are recorded here, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). This file tracks what the tool gained:
pipeline stages, runtime subsystems, services, formats, graphics features, online services and project infrastructure. It does not track per-game
compatibility; that lives in the generated [support matrix](docs/reference/support-matrix.md) and [online support](docs/reference/online-support.md) tables.

Napple has no hand-curated releases yet. Every commit to `main` that touches code publishes a release tagged `v0.1.<workflow run number>` (see
`.github/workflows/release.yml`; the `0.1` part is `MAJOR_MINOR` in that file, and docs- or site-only commits are skipped), so a version number identifies a
build, not a feature set. Until a stable version scheme exists, entries are grouped into dated milestones, newest first. Detailed per-change notes (what was
done, why, what was verified and what was not) are indexed in [docs/archive/changes/README.md](docs/archive/changes/README.md).

## Platform coverage (%Analysis)

The %Analysis score estimates how much of the Switch platform the pipeline and runtime cover (AOT rewrite, loader, kernel svcs, HLE IPC services and their
command depth, NVN API, Maxwell shader ISA, GPU formats, nvdrv, audio, input, containers, saves, applets, media). It is game-agnostic. Recorded snapshots:

| Date | Overall | Context |
|---|---|---|
| 2026-10-24 | 81.2% | first snapshot |
| 2026-10-24 | 86.8%, then 90.8% | services, TIPC, NSZ and DLC work |
| 2026-10-26 | 91.5%, then 94.1% | kernel scheduling, HID rings, f64 ISA, media services, surface ops |
| 2026-10-27 | 95.1%, 96.8%, then 98.0% | applets host, IPC depth, shader stages, content formats |
| 2026-10-28 | 97.8% | applets on title, mods in the runtime, NVDEC / VIC preview playback |
| 2026-10-29 | 98.1% | recorder race fix, software tessellator, nvdrv nodes, VP9 / NVJPG / MSENC |
| 2026-11-01 | 98.2% | IPC depth now measured from reference fixtures |
| 2026-11-02 | 98.3%, then 98.9% | views, raster state, AArch32 ordering, IPC reference audit |
| 2026-11-02 | 99.3% | host network provider, Napple Online services, IPC command depth 98% |
| 2026-11-02 | 99.3% | latest, after the Napple Online account, social and overlay work |

## Unreleased

### Added
- Napple Studio redesigned: one fixed-size translucent window that shows only the library, an Add / Build side panel for the whole flow (reading, include, Mac or iOS device options, progress with a live log, done, failed), Play with Debug (game plus a console window with its live log), required first-launch keys, and an eight-tab Settings window (key sets create/delete, iPhone & iPad signing and devices, Mii, Amiibo, Online, engine versions with install from GitHub releases and remove, last build log) ([note](docs/archive/changes/2026-11-03-studio-redesign.md)).
- Two instances on one Mac can play peer to peer through a local `napple-online` and a `napple-room`: each instance's room virtual address is its
  public address (NAT check "local IPv4" field and virtual port, trusted only for clients on the server's machine; `NAPPLE_ONLINE_NATCHECK_LOCALIP`);
  verified with a Mario Kart 8 Deluxe pair that matches, connects, votes a course and reaches the race countdown
  ([note](docs/archive/changes/2026-11-03-mk8d-online-race-2026-11-03-r12.md)).
- `NAPPLE_NET_LOG=2` dumps the first bytes of every datagram.
- iOS memory: opt-in disk-backed guest memory (`NAPPLE_IOS_SWAP=1`, default off) using unlinked file-backed mappings, which do not count in the process
  footprint (measured on macOS and the Simulator: 2 GiB dirtied = 2 MB vs 2050 MB anonymous); memory-pressure trimming (kernel pressure source,
  available-memory poll, UIKit warning); `extended-virtual-addressing` requested next to `increased-memory-limit` for paid accounts; footprint probe
  (`tools/swap_probe.c`). Nothing verified on a physical device yet
  ([note](docs/archive/changes/2026-11-03-ios-memory-swap.md), [design](docs/architecture/memory-swap.md)).

### Changed
- `napple-online` tells a joiner about every participant already in a verbose gathering (Notification 3001) and answers MatchmakeExtension 49.
- LDN Scan takes about 400 ms like the console's instead of answering at once (`NAPPLE_LDN_SCAN_MS`).

## 2026-11-02 to 2026-11-03: repository cleanup and public site

### Changed
- README and docs refocused on the tool (pipeline, subsystems, coverage, Napple Online, getting started); results are described without internal
  workflow vocabulary.
- Change-note file names in `docs/archive/changes/` lose their former prefix and follow `<date>-<branch>.md`; every link follows.
- Landing page rewritten around the recompilation toolchain (pipeline, coverage, Napple Online, get started, legal notice) with no per-title content; new
  `compat.html` renders per-title status and online support client-side from a generated `site/compat.json` with a static fallback; the terms, privacy
  and report pages share the new navigation and footer (legal text unchanged).
- `tools/support` generates the site's compat data and coverage block; `--check` also checks site links.
- Personal default paths in the golden, e2e and risk tooling replaced by `NAPPLE_TESTING_MATERIAL` / `NAPPLE_WORK`.

### Removed
- `Cargo.lock` files (root and `fuzz/`) are no longer tracked: cargo resolves them locally, CI cache keys hash the `Cargo.toml` files and no build uses
  `--locked`; OpenTofu provider locks stay pinned. `.gitignore` extended (lock files, logs, secrets, `.env`, editor data, local agent configuration).
- `CODE_OF_CONDUCT.md` and `SECURITY.md`; security reports go through the section in `CONTRIBUTING.md` (private GitHub security advisory or the
  maintainer).
- Personal tooling (`tools/dev`, the soak campaign driver), game screenshots, the video kit and `.github/CODEOWNERS`.

## 2026-11-02 to 2026-11-03: Napple Online

### Added
- Host network provider (`NAPPLE_NET=host`): BSD sockets, host resolver, TLS with the host trust store plus guest-imported CAs, nifm from the host's
  connectivity, LDN over the LAN; the console vendor's domains are never resolved unless a redirect hook claims them; `NAPPLE_NET_LOG`
  ([note](docs/archive/changes/2026-11-02-net-host-provider-2026-11-02-r6.md)).
- `napple-room`: a relay server (own protocol, std only) and runtime client (`NAPPLE_ROOM`) for LAN and LDN play between Napple instances over the
  internet, with `/healthz` and `/rooms` ([note](docs/archive/changes/2026-11-02-net-rooms-2026-11-02-r7.md)).
- `napple-online`: Napple-run accounts and tokens and a NEX game server (PRUDP Lite over WebSocket, RMC, Kerberos tickets, Authentication, Secure
  Connection, matchmaking, NAT traversal, Ranking, Pia NAT check), plus runtime glue (`NAPPLE_ONLINE_SERVER`) that redirects game-server names to it,
  blocks other vendor names and ends TLS locally; verified by reaching the online menu of a title such as Mario Kart 8 Deluxe
  ([note](docs/archive/changes/2026-11-02-napple-online-2026-11-02-r7.md)).
- `napple-account`: the Napple Account server with email + password sign-up confirmed by emailed codes (SES, SMTP and dev mailers, HTML and plain-text
  templates), devices, Ed25519 JWT + JWKS, end-to-end save-key exchange, cloud-save metadata with presigned S3 URLs, reserved names and admin grants
  ([note](docs/archive/changes/2026-11-03-account-auth-ses-2026-11-03-r12.md)).
- Profiles: unique nicknames, bio, links, icon, friend code and a sanitized photo pipeline with filesystem and S3 stores; `napple-visibility` crate with
  one `can_view` rule table ([note](docs/archive/changes/2026-11-03-account-profiles-2026-11-03-r12.md)).
- `napple-social`: friends, presence, invites and notifications (REST + WebSocket, SQLite), five visibility tiers, tier-aware search, request rules,
  lobbies with ghost aliases, recent players, reports and stats ([note](docs/archive/changes/2026-11-03-social-tiers.md)).
- End-to-end encrypted cloud saves (`napple-cloud`): Keychain store, HPKE key wrap, 24-word recovery, deterministic AES-256-GCM blobs, sync engine;
  runtime pulls before mount and pushes after commit; `napple account` and `napple saves` CLI
  ([note](docs/archive/changes/2026-11-02-cloud-saves-client-2026-11-02-r8.md)).
- `friend:u` backed by `napple-social` through `napple-online` (friend list, presence with join tokens, notifications); the NEX server's
  friends-playing query answers from reported friends.
- Napple Online client core: shared Keychain-wrapped sign-in, auth/profile/social client, profile mapping and a C API; a signed-in Mac plays online as
  its Napple profile ([note](docs/archive/changes/2026-11-03-online-tieup-2026-11-03-r12.md)).
- In-game overlay Online tab: account and profile screens, social sub-tabs (friends, requests, add, recent, blocked, stats), toasts and keyboard-bar
  text input.
- `napple-online` identity: account JWT logins, ghost aliases, a 30-day match log for report lookups, encounters and stats reported to `napple-social`.
- Servers on AWS for `pretend.napple.lat`: OpenTofu bootstrap and servers stacks (EC2, EIP, data volume, ECR, IAM, Route 53, S3 buckets, SES send
  permission), Docker images and a Compose stack behind Caddy TLS, a `deploy-servers` workflow (skips without AWS secrets, OIDC trust, SSM roll,
  smoke test) and a native local test stack ([note](docs/archive/changes/2026-11-02-napple-infra-2026-11-02-r7.md)).
- `napple.lat` DNS pointed at GitHub Pages (`www.napple.lat`).
- Napple Online terms, privacy and report policy, as docs and site pages
  ([note](docs/archive/changes/2026-11-03-legal-pages-r12.md)).
- Generated per-game online support table from `compat/online.json` (docs, README block, site JSON).

### Changed
- `ssl`: imported server CAs and the verify option reach the provider; non-blocking connections via `SetIoMode`, would-block reads and writes answer
  `ResultWouldBlock`.
- `ldn`: every provider node in NetworkInfo, state-change events, advertise data and accept policy accepted once the access point is open, Finalize
  ends a hosted or joined network.
- `bsd` replies use the console's errno numbers; an empty `Poll` with a timeout waits it out when the provider has sockets.
- `nifm`: `GetCurrentIpAddress` in network byte order, `GetCurrentIpConfigInfo`, per-installation pseudo device id; station URLs carry the NAT-check port.
- Paid plans and billing were dropped from the account server; storage limits are configuration.

### Fixed
- Applet storage channels with no data answer `NoDataInChannel`; unimplemented applets end as canceled; every error applet mode is decoded
  ([note](docs/archive/changes/2026-11-02-error-applet-output-2026-11-02-r6.md)).
- `nifm` Cancel keeps a rejected request's result and the offline NetConnect applet returns the real output
  ([note](docs/archive/changes/2026-11-02-nifm-netconnect-2026-11-02-r5.md)).
- Cloud-save uploads send the signed checksum headers that S3 requires on presigned PUTs.

### Security
- Reserved-name rules (nicknames, presence text, invite messages) applied across account and social services; internal routes hidden behind Caddy
  and protected by service tokens.

## 2026-11-01 to 2026-11-03: platform depth rounds

### Added
- IPC reference fixtures (switchbrew command tables) for the application-facing services and the `test_ipc_reference` runner; %Analysis measures IPC
  depth from them ([note](docs/archive/changes/2026-11-01-ipc-depth-hid-2026-11-01-r2.md)).
- A row for every application-reachable command of hid (148 of 148), am, system and offline services; several hundred documented-default rows later
  turned into real behaviour: fsp-srv (RomFS as IFileSystem, access-checked rows, save owner lists), acc, aoc, vi, caps, time, pdm, lm, ngc, set:sys
  settings store, notif:a alarms, pctl PIN and rating rows, bcat and npns local state, ssl, nifm, bsd, sfdnsres, ldn, olsc, btm, ovln. IPC command
  depth went from 92% to 98% ([note](docs/archive/changes/2026-11-02-ipc-depth-95-2026-11-02-r10.md)).
- OnlineProvider hooks for friend images and requests, nifm request status, account ID tokens and olsc; the null provider answers like an offline
  console.
- NVN: the 14 entry points titles look up beyond the catalog (537 of 537), AArch32 signatures for every entry point, guest-visible object sizes kept
  within the SDK's (`NVN_MIN_*`, `t_obj_canary`) ([note](docs/archive/changes/2026-11-01-nvn-object-layouts-2026-11-01-r3.md)).
- Graphics: copy-backed texture views for views Metal cannot make, integer sampled textures of every dimension, D24 / D24S8 readback, separate texture
  and sampler handles, stencil sampling of cube views, viewport swizzles (per viewport), provoking vertex last, viewport / scissor / depth-range arrays
  with layer and viewport-index outputs and layered passes ([note](docs/archive/changes/2026-11-02-gfx-multi-viewport-2026-11-02-r6.md)).
- Shader corpus tooling: `tools/gpu/scan_stages` reads RomFS images, SARC, Yaz0 and zstd and extracts a title's programs for the translator corpus.
- nvdrv GPU, nvdisp and NVDEC / VIC / NVJPG / MSENC structures checked against public sources and pinned by tests, typed GPU ioctls routed to node
  cores ([note](docs/archive/changes/2026-11-02-nvdrv-public-layouts-2026-11-02-r10.md)).
- NVJPG header-less decode from programmed tables and the entropy segment.
- Movie AAC (AudioToolbox) and VP8 (libvpx) host decoders, ARGB output; audout per-stream gain; audin uses the microphone when allowed.
- Title legal information extracted at build time and shown by the offline web applet; offline web document kinds.
- Frame-timed input scripts; `NAPPLE_THREAD_STATS`, `NAPPLE_ARB_TRACE` and audio dump diagnostics.

### Changed
- Scheduler follows Horizon rules: equal-priority rotation only at priority 59, a preempted thread keeps the head of its queue, the main thread starts on
  its npdm ideal core ([note](docs/archive/changes/2026-11-01-sched-applets-2026-11-01-r1.md)).
- 65 IPC rows renumbered or renamed to match the reference ([note](docs/archive/changes/2026-11-02-ipc-reference-audit-2026-11-02-r4.md)).
- IPC domain object ids are reused after close.
- Audio renderer performance buffer uses the real format 1 / 2 layouts; npad styles report device-type bits; bcat reads the console's delivery cache
  layout.

### Fixed
- The AOT rewrite emits guest exclusive pairs with acquire / release ordering, fixing a start-up race in job-claim spinlocks
  ([note](docs/archive/changes/2026-11-01-acnh-race-atomics-2026-11-01-r3.md)).
- AArch32: every lifted exclusive gets acquire / release, `lda` / `stl` map to `ldar` / `stlr` (the interpreter treats them as seq_cst with alignment
  faults); `svcGetInfo` 19 / 27 per reference ([note](docs/archive/changes/2026-11-02-core-ordering-kernel-2026-11-02-r4.md)).
- Translator: GLSLC geometry vertex handles, per-sample inputs under sample shading, TEX.3D AOFFI, TLDS.MS, TXQ of multisampled bindings, TLD4.PTP,
  LEA.HI constant-buffer form; tessellation output-primitive decode.
- Binary pipeline archive saves no longer disabled by a no-op serialize; a blit after a folded clear flushes it.
- Clean guest exit from the `nn::init` finalizer.

## 2026-10-27 to 2026-10-29: applets, mods, media and rendering options

### Added
- Generic library applet host (handler registry, firmware applet table, interactive channel, inline swkbd), HLE web / offline web and album applets,
  AppKit sheets for controller, error, player select, web (WKWebView) and album applets, live swkbd texts and per-profile account rows
  ([note](docs/archive/changes/2026-10-27-applets-host-2026-10-27.md)).
- Mii data and services from the firmware (default Miis, shared database, `mii:u` / `mii:e`), the firmware Mii Maker applet as a child process or
  in-process library; amiibo loader and IR camera tab in the overlay ([note](docs/archive/changes/2026-10-26-mii-firmware.md)).
- Mods: the `.napmod` format (IPS / IPS32 / pchtxt patches, cheats, RomFS overlay, config, shader files, trust model), `napple mods` CLI,
  `napple build --mods` before the AOT rewrite, merged RomFS overlay and a per-frame cheat VM in the runtime
  ([note](docs/archive/changes/2026-10-27-mod-framework.md)).
- Render scale 10..800%, integer / stretch / crop fit, built-in FSR-like and CAS upscalers next to MetalFX spatial, per-title scale rules
  ([note](docs/archive/changes/2026-10-27-render-scale-2026-10-27.md)).
- Present rate decoupled from game speed (`fps_mode` game / display / custom / uncapped).
- Geometry, tessellation and transform-feedback stages: translator side, replay (compute-emulated geometry, tessellation via TCS kernel and
  `drawPatches`), a software tessellator for isolines, TES transform feedback and GS after tessellation, stage parameters from the GLSLC control section
  ([note](docs/archive/changes/2026-10-28-gfx-stages-complete-2026-10-28-b8.md)).
- MSAA views and resolves, ResolveDepthBuffer, depth / stencil views, the last 18 rare Maxwell ops and stencil texture bindings.
- Media: nvdrv multimedia engines over a DecoderBackend (NVDEC H.264 via VideoToolbox, VP9, VIC, NVJPG decode and encode, MSENC), portable JPEG codec,
  caps album images, Vorbis / Opus movie audio ([note](docs/archive/changes/2026-10-28-nvdrv-media-devices-2026-10-28-b8.md)).
- State-tracked `nvhost-gpu`, `nvhost-as-gpu` and `nvhost-ctrl-gpu` ioctls.
- Containers: NSZ / XCZ zstd forms checked against the real writer, NCA0 / NCA2, IVFC verification, personalized tickets in the build pipeline,
  HtmlDocument extraction ([note](docs/archive/changes/2026-10-27-content-real-2026-10-27.md)).
- IPC depth: about 137 more stub rows with real behaviour (offline services, btm, hidbus, ldn, ns, ncm, sfdnsres, ngc, push), network rows through
  OnlineProvider ([note](docs/archive/changes/2026-10-27-ipc-depth-rest-2026-10-27.md)).
- Fuzzing: seeded-mutation tests and cargo-fuzz / C fuzz drivers for untrusted-input parsers; 26 findings fixed with regression tests
  ([note](docs/archive/changes/2026-10-27-fuzz-hardening-2026-10-27.md)).
- GPU title scanner (`tools/gpu/scan_stages.py`), golden-frame harness (`tools/golden`), end-to-end runs (`tools/e2e`), one system data root.
- Contributor kit: DCO sign-off and legal rules in `CONTRIBUTING.md`, issue forms, PR template, contributor guides.

### Changed
- Studio gained Mii, Amiibo and Online panes, a run log and an engine picker; camera and microphone entitlements.
- Kernel: timer preemption of spinning guest threads; post-mortem svc ring.

### Fixed
- Command-list handles by slot and generation with locked recording state, removing a recorder race (`NAPPLE_GFX_CBTRACE` tracer)
  ([note](docs/archive/changes/2026-10-28-gfx-recorder-flake-2026-10-28-b8.md)).
- Command lists live as long as their command memory; block-compressed render targets copied back; D24 / D24S8 uploads; integer MSAA resolve
  ([note](docs/archive/changes/2026-10-26-gfx-correctness-2026-10-26-macos1.md)).
- Control-section length decode; a save-commit race; deterministic `a32jit` output.

## 2026-10-25 to 2026-10-26: AArch32, releases and the slim pass

### Added
- AArch32 support: an interpreter core, then an ahead-of-time AArch32 to AArch64 lifter that builds `a32jit.dylib` (signable, no runtime JIT)
  ([note](docs/archive/changes/2026-10-25-mk8d-recompiler.md)); later full lifting with no interpreter in the shipped runtime, `napple build` always
  lifting AArch32 titles and `napple rewrite a32 --coverage` ([note](docs/archive/changes/2026-10-26-a32-full-2026-10-26-macos1.md)).
  Verified on a title such as Mario Kart 8 Deluxe.
- Updates and DLC on the 64-bit path with an in-place RomFS map.
- Per-commit release pipeline (`v0.1.<run number>`), GitHub Pages site and docs site (mkdocs), compat issue template, CI tests workflow with a docs check.
- Bit-exact double precision in the shader translator (DADD / DMUL / DFMA / DSETP / DMNMX / DSET, 64-bit conversions)
  ([note](docs/archive/changes/2026-10-26-f64-isa.md)); SULD / SUST / SUATOM / SURED as Metal image calls.
- Generated support matrix, feature-support table and site JSON with a `--check` staleness guard.
- Runtime depth: stub IPC rows made real for hid, bcat, audio, psm, lbl, settings, apm, pdm, pctl, aoc and parts of other services.
- Kernel scheduling and HID ring fixes; rewriter jump-table survey and size picks
  ([note](docs/archive/changes/2026-10-26-nx-rewriter-2026-10-26-nx.md)).
- iOS: alerts via `UIAlertController`, the iOS Simulator runs, profiling and memory logging.

### Changed
- Napple Studio rebuilt around one Liquid Glass window and a library of built games, with iOS install and IPA export
  ([note](docs/archive/changes/2026-10-26-studio-redo-2026-10-26.md)); new app icon.
- Slim pass: one clang-format / rustfmt run, compiler and clippy warnings to zero, dead code removed, shader cache cap, `napple clean`, Makefile
  dependency tracking ([note](docs/archive/changes/2026-10-26-code-slim.md)).
- Docs restructured into architecture, guides, reference, process and archive ([note](docs/archive/changes/2026-10-26-docs-consolidate.md)).
- LICENSE and repository hygiene files ([note](docs/archive/changes/2026-10-26-repo-hygiene.md)).

## 2026-10-23 to 2026-10-24: services, correctness rounds and iOS

### Added
- 24 new HLE services and deeper commands, TIPC request dispatch, `ngc:u`, `audrec:u`, `audin:u`, `ncm` tables, `caps`, audren performance metrics
  ([note](docs/archive/changes/2026-10-24-services-unbuilt.md)).
- Loader and kernel: guest IPC, code memory, NRO loading, host1x channels, save data management
  ([note](docs/archive/changes/2026-10-24-loader-kernel.md)).
- DLC mounting (`aoc:u`), delta RomFS, firmware system data archives served by `fsp-srv`, placeholder shared font when fonts are missing.
- NSZ / XCZ ingest (zstd NCZ, streaming and block forms) ([note](docs/archive/changes/2026-10-24-nsz-zstd-2026-10-24-plan11.md)).
- Error, player select and controller applets; swkbd configuration parsing.
- Shader ISA gaps (LDS / STS, TLD, TXD, atomics, VOTE, IMAD, LEA and more), more NVN entry points and pipeline features, NVN getters / setters,
  transform feedback and GL-sync entries ([note](docs/archive/changes/2026-10-24-shader-isa.md)).
- I3DL2 reverb fixes; VideoToolbox H.264 decoder for the movie library.
- iOS / iPadOS 26+ target: modules as signable Mach-O dylibs, UIKit frontend with an on-screen controller, profile signing, IPA and device install
  ([note](docs/archive/changes/2026-10-24-ios-support.md)).
- Input remapping, transactional save data, drift-free frame pacing, settings overlay, checks of the packaged `.app`
  ([note](docs/archive/changes/2026-10-23-input-audio-runtime-round2.md)).
- The Rust workspace builds off macOS.

### Fixed
- Missing vertex formats (RGB10A2 signed-normalized and the integer-to-float family) and compute uniform buffer addresses
  ([note](docs/archive/changes/2026-10-23-graphics-round2.md)).
- Dirty-tracking protection kept per host page, fixing write faults when 4 KiB guest pools share a 16 KiB host page
  ([note](docs/archive/changes/2026-10-23-crash-and-gfx-round3.md)).
- Finalized command-buffer recordings kept alive while still called; occlusion query counts; `LDC.64` with an aliased address register
  ([note](docs/archive/changes/2026-10-23-smo-round.md)).
- Pipeline-build hang; rewriter refuses calls inside exclusive windows and prunes landing pads on rollback.

## 2026-10-21 to 2026-10-22: runtime, graphics and packaging

### Added
- Generic A64 NSO rewriter (`napple-rewrite`): virtual x18 and TLS registers in per-thread slots, `svc` and counter reads through a host gate, exclusive
  windows, `br x18` jump-table recovery, structural verification ([note](docs/archive/changes/2026-10-21-rewriter.md)).
- Game-agnostic native runtime: module loader driven by data tables, Horizon kernel surface (handles, threads, sync, memory), HIPC / CMIF IPC with
  domains and `sm:`, the first HLE services ([note](docs/archive/changes/2026-10-21-runtime-core.md)).
- Audio, input and frontend static libraries: audren with mixer and effects, audout, hardware Opus, HID shared memory, GameController input and
  rumble, window, overlay and settings ([note](docs/archive/changes/2026-10-21-audio-input-frontend.md)).
- `libnapple_gfx`: NVN on Metal (510 of 523 entry points at first), a native Maxwell to MSL shader translator, Tegra block-linear layouts, BC decode,
  headless tests ([note](docs/archive/changes/2026-10-22-graphics.md)).
- All components linked into one runtime with first rendered frames; VP9 movies decoded on the host with libvpx
  ([note](docs/archive/changes/2026-10-22-integration.md)).
- `napple build`: one-process pipeline from base game, updates and DLC to a locally signed `.app`, with firmware fonts and a launcher
  ([note](docs/archive/changes/2026-10-22-packaging.md)); Studio drives it end to end.

## 2026-10-20: ingest, build plan and first tools

### Added
- `napple-core` ingest: XCI / NSP containers, key derivation from `prod.keys`, NCA3 decryption, tickets, ExeFS and RomFS
  ([note](docs/archive/changes/2026-10-20-ingest.md)).
- `napple plan` / `napple decomp`: CNMT parsing, base / update / DLC selection, NSO loader and the A64 rewrite-site scanner
  ([note](docs/archive/changes/2026-10-20-decomp-plan.md)).
- Shared signatures (`napple-sigs`): schema, stores, matcher, resolver and an opt-in share flow.
- Per-stage compatibility matrix (`napple-compat`) and NPDM / NACP parsers.
- Napple Studio (SwiftUI): key sets, drop zone, plan, build and compat banner.
