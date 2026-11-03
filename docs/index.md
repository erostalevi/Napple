# Napple documentation

Napple is an ahead-of-time recompiler and runtime that turns a Switch game you own into a native app for macOS on Apple Silicon and for iOS / iPadOS.
At build time it ingests the game, rewrites its AArch64 code and packages a locally signed `.app`; at run time a Horizon kernel and HLE service layer hosts the code, and NVN calls run on Metal.
Napple Studio is the graphical front end; mods and the optional Napple Online service are add-ons. The repository front page is the [README](../README.md).

## Architecture

| Page | What it covers |
|---|---|
| [Overview](architecture/overview.md) | Build-time pipeline (ingest XCI/NSP/NSZ, NCA crypto, updates and DLC, NSO analysis, signature resolve, AArch64 rewrite, packaging), run-time layout, source tree, design decisions |
| [Runtime data and module tables](architecture/module-format.md) | Data directory, NTB tables, app bundle and per-title patch table, RomFS modes, launch contract |
| [Rewriter output format](architecture/rewrite-format.md) | Rewrite of x18, TPIDR, svc, counters and page size: patched images, key page, gate, fixups, sites |
| [Runtime services and kernel](architecture/runtime-services.md) | Loader, Horizon kernel (svc), HLE IPC services, saves, applets, nvdrv, media decode, frame pacing |
| [Graphics](architecture/gfx.md) | NVN on Metal, the Maxwell shader translator, render scale and upscaling, geometry / tessellation / transform feedback |
| [Audio, input, frontend](architecture/audio-input-frontend.md) | The audio, input and frontend libraries and the API the runtime calls |
| [AArch32 path](architecture/aarch32.md) | 32-bit titles: the build-time lifter and its runtime support |
| [iOS and iPadOS](architecture/ios.md) | AOT-only target, signed dylib modules, signing and install, memory layout, Simulator limits |
| [Studio protocol](architecture/studio-protocol.md) | How Napple Studio drives the `napple` CLI |
| [Compatibility matrix format](architecture/compat-format.md) | `compat/matrix.json`, stages, statuses |

## Guides

The [guide index](guides/README.md) lists every guide.
- Build and run: [Napple Studio](guides/studio.md), [bring up a new title](guides/add-game-support.md), [debugging](guides/debugging.md), [hardware simulators](guides/hardware-simulators.md) (amiibo, IR camera).
- Contributor guides: [add an IPC service](guides/add-ipc-service.md), [add an svc](guides/add-svc.md), [add an NVN function](guides/add-nvn-function.md), [add a Maxwell shader op](guides/add-maxwell-shader-op.md), [scan a title's GPU stages](guides/gpu-title-scan.md), [add signatures](guides/add-signatures.md), [network providers](guides/network-providers.md), [testing](guides/testing.md), [graphics testing](guides/testing-graphics.md), [fuzzing](guides/fuzzing.md).
- Community contributions (no code needed): [report a bug](guides/contributing-report-a-bug.md), [document a title](guides/contributing-document-a-title.md), [report a graphics bug](guides/contributing-report-a-graphics-bug.md), [write a mod or fix](guides/contributing-write-a-mod.md), [labels, Discussions, conduct](guides/contributing-community.md); the legal rules and sign-off are in [CONTRIBUTING.md](../CONTRIBUTING.md).

## Online

Napple Online is optional and runs on Napple's own servers (`pretend.napple.lat`), never on Nintendo's.

| Page | What it covers |
|---|---|
| [Napple Online](guides/napple-online.md) | Overview, boundaries, the game server (`napple-online`), how a title gets online |
| [Account and profiles](guides/napple-account.md) | `napple-account`: email-code sign-up, devices, tokens, up to 8 profiles, reserved names |
| [Friends and visibility tiers](guides/napple-social.md) | `napple-social`: friends, presence, visibility tiers 1-5, search, lobbies, reports |
| [Cloud saves](guides/cloud-saves.md) | End-to-end encrypted cloud saves: devices, approval and recovery, sync and conflicts |
| [Client and overlay](guides/napple-online-client.md) | The Mac-side client, shared credentials, the in-game overlay Online tab |
| [Rooms](guides/online-rooms.md) | `napple-room`: local wireless and LAN play between Napple instances |
| [End-to-end run](guides/online-e2e.md) | The scripted Online end-to-end check |
| [Deploy the servers](guides/deploy-servers.md) | AWS deployment at `pretend.napple.lat`, DNS, the local test stack |
| [Legal documents](legal/README.md) | Napple Online terms, privacy and report policy |

## Reference

| Page | What it covers |
|---|---|
| [Subsystem map](reference/subsystems.md) | Code, tests and debug switches per subsystem |
| [CLI](reference/cli.md) | `napple` and the runtime command line |
| [Environment variables](reference/environment-variables.md) | Every `NAPPLE_*` variable read by the code |
| [System data folders](reference/system-data-folders.md) | The support root: Mii database, amiibo store, fonts, `system.json` |
| [nvdrv layouts](reference/nvdrv-layouts.md) | GPU nodes, nvdisp and host1x media engine ioctl / method layouts with their public sources |
| [Glossary](reference/glossary.md) | Horizon, NSO, NCA, BKTR, NVN, Maxwell and the rest |
| [Tooling](reference/tooling.md) | REA, Ghidra and the JDK used for signatures |
| [Fuzz findings](reference/fuzz-findings.md) | Defects found by the fuzzers and their fixes |
| [Feature support](reference/feature-support.md) | Generated coverage of the platform by domain and what is not implemented yet ([JSON for the site](reference/support-data.json)) |
| [Open work markers](todo.md) | Deferred code-level work |
| [Symptoms](symptoms.md) | Symptom, cause, fix, and where it was found |

## Mods

- [Mods and fixes (`.napmod`)](mods/README.md): the shareable mod format, [authoring a fix](mods/authoring.md), [format reference and trust model](mods/format.md), [integration with the build and the runtime](mods/integration.md).

## Titles and compatibility

- [COMPATIBILITY.md](../COMPATIBILITY.md) (the stage matrix), the [title notes](titles/README.md) ([Link's Awakening](titles/01006BB00C6F0000.md), [Super Mario Odyssey](titles/0100000000010000.md), [Animal Crossing: New Horizons](titles/01006F8002326000.md), [Mario Kart 8 Deluxe](titles/0100152000022000.md)), and the generated [support matrix](reference/support-matrix.md), [online support](reference/online-support.md) and [catalogue field reference](reference/catalogue-howto.md).

## History

- [Change archive](archive/changes/README.md): dated notes of every change.
- [Process archive](process/archive/README.md)
