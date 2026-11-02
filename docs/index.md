# Napple documentation

Napple turns a Switch game you own into a native Apple Silicon app (ahead-of-time ARM64 rewrite, Horizon HLE runtime, NVN on Metal). This is the documentation of the project;
the repository front page is the [README](../README.md) and the title status is in [COMPATIBILITY.md](../COMPATIBILITY.md).

## Start here by role

**User** (you want to build and run a game)
1. [README](../README.md): what Napple is, requirements and the legal notes.
2. [CLI reference](reference/cli.md) and [Studio protocol](architecture/studio-protocol.md) for the build commands.
3. [Per-title notes](titles/README.md) for the four reference titles and how to build their test data.
4. [Symptoms](symptoms.md): something went wrong, look up the symptom.

**Contributor** (you want to change the code)
1. [Architecture overview](architecture/overview.md), then the page of the subsystem you touch.
2. [Contributor guides](guides/README.md): add a service, an svc, an NVN function, a Maxwell op, a game, a signature; [debugging](guides/debugging.md); [testing](guides/testing.md); [network providers](guides/network-providers.md); [hardware simulators](guides/hardware-simulators.md); [Napple Studio](guides/studio.md).
   Community contributions (no code needed): [report a bug](guides/contributing-report-a-bug.md), [document a title](guides/contributing-document-a-title.md), [report a graphics bug](guides/contributing-report-a-graphics-bug.md), [write a mod or fix](guides/contributing-write-a-mod.md), [labels, Discussions, conduct](guides/contributing-community.md); the legal rules and sign-off are in [CONTRIBUTING.md](../CONTRIBUTING.md).
3. [Subsystem map](reference/subsystems.md) (code, tests, debug switches per subsystem), [environment variables](reference/environment-variables.md), [glossary](reference/glossary.md).

**Maintainer** (you integrate and release)
1. [Parallel-session workflow](process/README.md).
2. [Change archive](archive/changes/README.md): the dated notes of every round.
3. [Tooling](reference/tooling.md) (REA, Ghidra) and the CI workflows in `.github/workflows/`.

## Contents

### Architecture
| Page | What it covers |
|---|---|
| [Overview](architecture/overview.md) | Pipeline, runtime, source layout, design decisions, diagrams |
| [Runtime data and module tables](architecture/module-format.md) | Data directory, NTB tables, app bundle, RomFS modes, launch contract |
| [Rewriter output format](architecture/rewrite-format.md) | Patched images, key page, gate, fixups, sites |
| [Compatibility matrix format](architecture/compat-format.md) | `compat/matrix.json`, stages, statuses |
| [Runtime services and kernel](architecture/runtime-services.md) | Every HLE service and svc with its status |
| [Graphics](architecture/gfx.md) | Design decisions and the runtime to gfx interface |
| [Audio, input, frontend interface](architecture/audio-input-frontend.md) | Library APIs the runtime calls |
| [AArch32 path](architecture/aarch32.md) | 32-bit titles: interpreter and recompiler |
| [iOS and iPadOS](architecture/ios.md) | AOT-only target, signing, install, Simulator results |
| [Studio protocol](architecture/studio-protocol.md) | How Studio drives the `napple` CLI |

### Guides
See the [guide index](guides/README.md).

### Reference
| Page | What it covers |
|---|---|
| [Subsystem map](reference/subsystems.md) | Code, tests and debug switches per subsystem |
| [CLI](reference/cli.md) | `napple` and the runtime command line |
| [System data folders](reference/system-data-folders.md) | The one support root: Mii database, amiibo store, fonts, `system.json` |
| [Environment variables](reference/environment-variables.md) | Every `NAPPLE_*` variable read by the code |
| [Support matrix](reference/support-matrix.md) | Generated per-title status per platform (macOS now, iOS deferred) |
| [Feature support](reference/feature-support.md) | Generated coverage by domain and what is not implemented yet ([JSON for the site](reference/support-data.json)) |
| [Catalogue field reference](reference/catalogue-howto.md) | Where each title fact is written and what the generated support tables read |
| [Glossary](reference/glossary.md) | Horizon, NSO, NCA, BKTR, NVN, Maxwell and the rest |
| [Tooling](reference/tooling.md) | REA, Ghidra and the JDK used for signatures |
| [Open work markers](todo.md) | Deferred work that used to be `TODO` comments in the source |

### Mods and fixes
- [Mods and fixes (`.napmod`)](mods/README.md): the shareable mod format, [authoring a fix](mods/authoring.md), [format reference and trust model](mods/format.md), [integration notes](mods/integration.md).

### Titles and symptoms
- [titles/](titles/README.md): [Link's Awakening](titles/01006BB00C6F0000.md), [Super Mario Odyssey](titles/0100000000010000.md), [Animal Crossing: New Horizons](titles/01006F8002326000.md), [Mario Kart 8 Deluxe](titles/0100152000022000.md).
- [Symptoms](symptoms.md): symptom, cause, fix, and where it was found.

### Media
- [YouTube video kit](media/youtube/README.md): script, call to action, shot list, titles and description.

### Process and history
- [Parallel-session workflow](process/README.md)
- [Change archive](archive/changes/README.md). Notes of the unattended rounds that the archive README does not list yet:
  - Batch 2026-10-27-b7: [applets on a title](archive/changes/2026-10-27-claude-applets-on-title-2026-10-27-b7.md), [gfx stages real](archive/changes/2026-10-27-claude-gfx-stages-real-2026-10-27-b7.md), [applets live](archive/changes/2026-10-28-claude-applets-live-2026-10-27-b7.md), [content real 2](archive/changes/2026-10-28-claude-content-real-2-2026-10-27-b7.md).
  - Round 2026-11-01-r1: [sched-applets](archive/changes/2026-11-01-claude-sched-applets-2026-11-01-r1.md), [gfx-msaa-views](archive/changes/2026-11-01-claude-gfx-msaa-views-2026-11-01-r1.md), [gfx-stages-live](archive/changes/2026-11-01-claude-gfx-stages-live-2026-11-01-r1.md), [sweep-small](archive/changes/2026-11-01-claude-sweep-small-2026-11-01-r1.md).
  - Round 2026-11-01-r2: [gfx-nvn-api](archive/changes/2026-11-01-claude-gfx-nvn-api-2026-11-01-r2.md), [gfx-shader-views](archive/changes/2026-11-01-claude-gfx-shader-views-2026-11-01-r2.md), [ipc-depth-hid](archive/changes/2026-11-01-claude-ipc-depth-hid-2026-11-01-r2.md), [ipc-depth-sys](archive/changes/2026-11-01-claude-ipc-depth-sys-2026-11-01-r2.md), [sweep-media-r2](archive/changes/2026-11-01-claude-sweep-media-r2-2026-11-01-r2.md).
  - Round 2026-11-01-r3: [acnh-race-atomics](archive/changes/2026-11-01-claude-acnh-race-atomics-2026-11-01-r3.md), [am-depth-applets](archive/changes/2026-11-01-claude-am-depth-applets-2026-11-01-r3.md), [gfx-replay-gaps](archive/changes/2026-11-01-claude-gfx-replay-gaps-2026-11-01-r3.md), [ipc-local-stubs](archive/changes/2026-11-01-claude-ipc-local-stubs-2026-11-01-r3.md), [nvn-object-layouts](archive/changes/2026-11-01-claude-nvn-object-layouts-2026-11-01-r3.md).
  - Round 2026-11-02-r4: [core-ordering-kernel](archive/changes/2026-11-02-claude-core-ordering-kernel-2026-11-02-r4.md).
