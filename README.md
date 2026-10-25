<div align="center">

# Napple

**Turn a Switch game you own into a native Apple Silicon app.**

Ahead-of-time ARM64 rewrite · Horizon HLE · NVN → Metal · no emulator, no JIT

![platform](https://img.shields.io/badge/platform-macOS%20%7C%20Apple%20Silicon-black)
![core](https://img.shields.io/badge/core-Rust%202024-orange)
![runtime](https://img.shields.io/badge/runtime-C%20%2B%20Objective--C-blue)
![studio](https://img.shields.io/badge/studio-SwiftUI-red)
![license](https://img.shields.io/badge/license-MIT-green)
![status](https://img.shields.io/badge/status-early%20%2F%20experimental-yellow)

</div>

---

## What is this?

The Switch and every Apple Silicon Mac are both ARM64. So instead of emulating the console, Napple **runs the game's own machine code natively** and
rewrites, ahead of time, only the few things macOS forbids: the reserved `x18` register, the thread-pointer register, `svc` system calls and the
timer-counter registers. Everything the game asks the console OS for is answered by a Horizon high-level-emulation runtime, and the GPU API (NVN)
is translated to Metal.

The result is a **standalone, locally signed `.app`** built from your own copy of the game. No JIT is needed, which also keeps the door open for iOS/iPadOS.

```
 your files: game (XCI / NSP / NSZ) + prod.keys + firmware
   │
   ├─ 1 ingest    container parse, NCA decrypt (your keys), ExeFS / RomFS, updates + DLC (BKTR patches), version & hash ID
   ├─ 2 analyze   NSO load, function discovery, patch-site scan
   ├─ 3 resolve   signature lookup (known signatures → reverse-engineering fallback)
   ├─ 4 rewrite   AArch64 rewriter: x18, TPIDR, svc, counters, 4K→16K page issues
   ├─ 5 package   .app = runtime + per-game patch table (ad-hoc signed on your machine)
   │
   └─ run         loader · Horizon kernel + services · NVN→Metal · audio · input · saves
```

Per-game knowledge lives only in **data** (signatures, patch tables), never in per-game code branches. Napple is meant to be game-agnostic.

## Highlights

- **Native speed.** Guest code executes directly on the CPU; there is no interpreter or recompiler at run time.
- **Horizon kernel + HLE services.** Threads, scheduling, memory, sessions and IPC, plus a broad set of system services, with offline answers for the online ones.
- **NVN → Metal.** A GPU layer with a Maxwell shader-ISA translator, NVN pipeline and format support, MSAA, indirect draws, query objects and more.
- **Audio and input.** Audio renderer (reverb, delay, compressor, limiter, Opus) over Core Audio; GameController input with rumble, keyboard and mouse mapping.
- **Saves, updates and DLC.** Save data lives in `~/Library/Application Support/Napple/<titleId>`. Base game, updates and DLC packs are merged in any container, in any order.
- **Napple Studio.** A small SwiftUI app: drop a game on the window, watch the pipeline run, install the result.
- **Compatibility matrix.** Every game and every pipeline stage is tracked with evidence, see [`COMPATIBILITY.md`](COMPATIBILITY.md).

## Legal, up front

- You need to **own the game** and supply **your own `prod.keys` and firmware**. Nothing is downloaded, bundled or committed. Ever.
- Keys, firmware and game files are needed **only at build time**. The produced app is self-contained, but it contains game-derived content, so it is **for your personal use only**. Napple does not condone sharing builds.
- The repository contains no game bytes, keys or firmware. The compatibility matrix is metadata only.
- Napple is not affiliated with or endorsed by Nintendo.

## Status

Napple is early and moving fast. Two titles currently boot all the way to their title screen / early gameplay on the runtime; neither is fully playable yet. See the
[compatibility matrix](COMPATIBILITY.md) for the per-stage truth.

The table is an **estimate of how much of the Switch platform surface the pipeline and runtime cover** (not per-game compatibility), produced by the `%Analysis` scanner:

| Area | Coverage |
|---|---|
| CPU: AOT A64 rewrite | 100% |
| Loader: NSO / NRO / modules / relocations | 100% |
| Kernel: svcs + scheduling | 99% |
| IPC: HLE service coverage | 97% |
| IPC: command depth (implemented vs. stubbed) | 45% |
| GPU: Maxwell shader ISA | 81% |
| GPU: NVN API surface | 81% |
| GPU: formats + pipeline features | 83% |
| Audio | 94% |
| Input | 99% |
| Content: containers, crypto, updates, DLC | 91% |
| Saves + storage | 100% |
| Library applets | 48% |
| Video / media decode | 61% |
| **Overall** | **≈ 87%** |

Coverage is not compatibility: a long tail of command depth, shader corner cases and GPU behaviour is exactly what real games trip over first.

## Download

Every commit to `main` publishes a release (`.github/workflows/release.yml`): **Napple-Studio.dmg** (drag to Applications; Studio sets up the engine and
`NAPPLE_HOME` / `NAPPLE_CLI` on first launch) and **Napple-macOS-arm64.zip** (Studio + `bin/` + `install.sh`). Landing page source: `site/` (GitHub Pages via `pages.yml`).

## Quick start

> Requires an Apple Silicon Mac, Xcode command line tools, a Rust toolchain (2024 edition), and your own keys and game.

```sh
# 1. build the toolchain
cargo build --release -p napple-cli
make -C runtime            # Horizon runtime
make -C packaging          # launcher

# 2. (optional) the GUI
studio/build_app.sh        # → studio/dist/NappleStudio.app

# 3. use the CLI
target/release/napple ingest <game.xci|.nsp|.nsz> -k prod.keys
target/release/napple plan   <game> -k prod.keys
target/release/napple decomp <game> -k prod.keys -o out/ --updates latest --dlc all
```

Or just open **Napple Studio**, give it your keys once, and drop a game onto the window.

Run the tests:

```sh
cargo test                 # Rust core
make -C runtime test       # runtime, on macOS
make -C runtime test-host  # portable kernel / loader / service tests, any POSIX host
```

## Repository map

| Path | What lives there |
|---|---|
| `crates/napple-core` | domain types, containers, NCA crypto, content/update/DLC handling |
| `crates/napple-rewrite` | the AArch64 rewriter |
| `crates/napple-sigs` | signature resolution and shared-signature support |
| `crates/napple-build` | `.app` packaging |
| `crates/napple-compat` | compatibility matrix tooling |
| `crates/napple-cli` | the `napple` command line |
| `runtime/` | loader, Horizon kernel, HLE services |
| `gfx/` | NVN → Metal, Maxwell shader translator |
| `audio/` · `input/` · `frontend/` | audio renderer, controllers, windowing and settings overlay |
| `studio/` | Napple Studio (SwiftUI) |
| `packaging/` | app launcher |
| `compat/` | compatibility data (`matrix.json`) |
| `docs/` | architecture, formats, protocols and per-change notes |

Start with [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Service coverage is listed in [`docs/runtime-services.md`](docs/runtime-services.md).

## Contributing

Issues and reports are welcome, especially compatibility results (stage, evidence, the services or NVN calls that were missing).
Please never attach game files, keys or firmware, to an issue or anywhere else.

## Credits and license

Napple stands on the shoulders of open projects: audio renderer formats and effect algorithms adapted from **Ryujinx.Audio**, and runtime structure and controller
approaches adapted from **MacNativeGames**, both MIT. Full notices are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) (and
[`gfx/THIRD_PARTY_NOTICES.md`](gfx/THIRD_PARTY_NOTICES.md)) and must be kept with any distribution.

Napple is released under the MIT license.
