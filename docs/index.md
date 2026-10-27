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
| [Environment variables](reference/environment-variables.md) | Every `NAPPLE_*` variable read by the code |
| [Support matrix](reference/support-matrix.md) | Generated per-title status per platform (macOS now, iOS deferred) |
| [Feature support](reference/feature-support.md) | Generated coverage by domain and what is not implemented yet ([JSON for the site](reference/support-data.json)) |
| [Glossary](reference/glossary.md) | Horizon, NSO, NCA, BKTR, NVN, Maxwell and the rest |
| [Tooling](reference/tooling.md) | REA, Ghidra and the JDK used for signatures |
| [Open work markers](todo.md) | Deferred work that used to be `TODO` comments in the source |

### Titles and symptoms
- [titles/](titles/README.md): [Link's Awakening](titles/01006BB00C6F0000.md), [Super Mario Odyssey](titles/0100000000010000.md), [Animal Crossing: New Horizons](titles/01006F8002326000.md), [Mario Kart 8 Deluxe](titles/0100152000022000.md).
- [Symptoms](symptoms.md): symptom, cause, fix, and where it was found.

### Process and history
- [Parallel-session workflow](process/README.md)
- [Change archive](archive/changes/README.md)
