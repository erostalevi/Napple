# Tooling (REA, Ghidra, JDK, gh)

Installed 2026-10-20 on macOS 27 / arm64 for the shared-signatures work. Nothing here edits any Claude/agent configuration.

| Tool | Version | How installed | Where |
|---|---|---|---|
| Node | 26.7.0 | already present (nvm) | `~/.nvm` |
| REA (`rea-agents`, MIT) | 6.7.0 | `npm install --prefix work/tools rea-agents` | `work/tools/node_modules/.bin/rea` (git-ignored) |
| Ghidra | 12.1.4 | `brew install ghidra` (it is a **formula**; there is no cask) | `/opt/homebrew/opt/ghidra/libexec` |
| JDK | OpenJDK 21.0.13 | pulled in by the Ghidra formula (`openjdk@21`) | `/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home` |
| GitHub CLI `gh` | 2.102.1 | `brew install gh` | `/opt/homebrew/bin/gh` (NOT authenticated, nothing pushed) |

`npx rea-agents setup` was deliberately NOT run (it edits agent configs). The npm install printed a warning that
the `frida` dependency has an install script that was not run; Frida features are unused here.

## Environment REA needs

```
export GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
work/tools/node_modules/.bin/rea doctor --json     # ghidra check: ok, provider 12.1.4, native decompiler arm64
```
`doctor` still reports `missing_analysis_engine` for Hopper/IDA/jadx/kuna and `config_drift` for agent
registrations: expected and irrelevant (only the Ghidra provider is used). `ReaCliReverser` auto-detects the two paths above (override with
`GHIDRA_INSTALL_DIR`, `JAVA_HOME`, `NAPPLE_REA_BIN`).

## Verified commands

Test binary: `clang -arch arm64 -O1 work/tooltest/t.c -o work/tooltest/t.bin` (small ARM64 Mach-O).

```
rea analyze work/tooltest/t.bin --provider ghidra --format json         # ~8 s; Mach-O, 4 procedures, 14 strings
rea search-bytes work/tooltest/t.bin "?? ?? ?? 0b" --provider ghidra --format json   # masked byte search, returns procedure
# headerless ARM64 code (what an NSO .text is), loaded at an address of our choice:
rea function work/tooltest/t.raw 0x0 --target-format raw-binary --architecture arm64 --entry-point 0x0 \
    --provider ghidra --format json
```
The last form returns function body ranges, assembly and decompiled pseudocode (`return param_1 + param_2 + 3;`).
This is what `ReaCliReverser` runs. Real data: on a 64 KiB slice of Link's Awakening `main` starting at a function
start, REA/Ghidra returned the function boundary in about 12 s (whole `napple sigs resolve` run).

## What REA/Ghidra can and cannot do here

Can: headless Ghidra through REA's `--format json` CLI; function boundaries, pseudocode, xrefs, byte search for
raw ARM64 images; Mach-O natively.
Cannot / limits:
- No Switch NSO loader: the NSO must be unpacked to raw code first (Napple does this) and loaded as `raw-binary`
  at a chosen address. Switch ELF/NSO structure, symbols (NSO files are stripped) and rodata strings are not known to Ghidra, so
  names come back as auto names (`FUN_...`/`entry`); the adapter treats those as "no name".
- The adapter analyses from a given function START (the image slice begins at the item). It cannot find where a
  function begins from the middle, and does not discover HLE service names; names must come from humans or from other builds' signatures.
- Cold start costs several seconds per call (JVM + Ghidra import); fine for per-item resolution, not for whole-module sweeps.
- Hopper/IDA providers are not installed (licensed products).

## Parallel sessions
See the [parallel-session workflow](../process/README.md).
