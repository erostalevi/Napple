# Mod framework: the `.napmod` format and tooling (2026-10-27, branch claude/mod-framework-2026-10-27)

Assignment `orchestrations/mod-framework.md` (batch 2026-10-27-polish, cloud Linux). Community mods and fixes that anyone can create and share. Everything here is the **portable core**; nothing is wired into the build, runtime or UI yet (status words: *tested* = unit/integration test passed on Linux, *unbuilt* = never compiled on macOS, but this crate has no macOS code).

## What was built
* New crate `crates/napple-mods` (in the workspace; dependencies `serde`, `serde_json`, `sha2`, `zip` without default features, `tempfile` for tests). `napple-cli` gets one line to register `napple mods ...`; `napple-build` was not touched.
* Format (`docs/mods/format.md`): `.napmod` = zip with `mod.json` (id, name, version, titleId, buildIds[], author, license, requires, components, description, thumbnail, reserved signature), `manifest.sha256`, `patches/` (pchtxt, IPS, IPS32 incl. RLE, EOF, truncate), `cheats/<BID16>.txt`, `files/` romfs overlay with `.delete` markers, `config.json`, `shaders/<hash>.msl`.
* Library: parse / validate / pack / inspect; apply to NSO text, rodata and data bytes with an apply report (hits, misses with reason, overlaps, skipped, before/after SHA-256, dry run); pchtxt <-> JSON <-> IPS conversion; romfs overlay resolution (order, deletes, conflicts, lookup); config merge; cheat decoder and interpreter over a `CheatMemory` trait; shader validation.
* CLI: `napple mods pack|inspect|validate|apply --dry-run|convert|build-id`.
* Hook: `apply_mods_to_module(build_id, &mut ModuleBytes, &[ResolvedMod]) -> ApplyReport` (and `dry_run_mods_for_module`), documented with the exact `napple-build` call site in `docs/mods/integration.md`.
* Trust model in code and docs: unsigned by default (reserved `signature`, an error if set), capabilities counted from the files and checked against `components`, code patches/cheats/shaders default off (`Selection`), SHA-256 manifest, never auto-download, path traversal / symlinks / encrypted / duplicate entries rejected at read time, key files an error, extracted-asset dumps a warning by size/type.
* Docs: `docs/mods/{README,format,authoring,integration}.md`, example mod `docs/mods/examples/synthetic-fix/` (synthetic title only) and `synthetic-module.bin` (generated and compared by a test), docs index entry, mkdocs nav entry, `docs/reference/cli.md` section.

## Decisions (no one to ask)
* **Offsets** are NSO *file* offsets (header included): module offset = offset - 0x100. This is the Atmosphere IPS convention and what pchtxt `@flag offset_shift 0x100` produces; records are stored with the shift folded in.
* **Truncation** in IPS is parsed and written but never applied to a module (reported as a `truncate_rejected` miss): resizing a module would move every address.
* **Cheats default off** like code patches (they write guest memory); romfs and config follow "mod enabled".
* **Declared components must match the files** (an undeclared patch or cheat is an error), so the capability list shown to users cannot be understated.
* Key files (`prod.keys` etc.) are an *error*; asset dumps (containers, modules, huge files or overlays) only a *warning*, as the assignment says.
* The romfs overlay tree is `files/` (assignment wording); cheats follow the Atmosphere file name (`<first 8 bytes of the main build id>.txt`).
* `pack` is deterministic (sorted entries, fixed timestamps, mode 0644), so a package hash can be published and reproduced.
* The cheat opcode subset is Atmosphere code types 0 (store), 1 (begin conditional) and 2 (end / else); everything else is decoded as "unsupported" and stops the decoder with a warning.

## Tests
`cargo test -p napple-mods`: **32 tests**, all passing.
| Group | Tests |
|---|---|
| IPS / IPS32 (plain, RLE, EOF, EOF-lookalike offset, truncate, malformed, corpus) | 5 |
| pchtxt (blocks/shift/comments/builds/@stop, round trips pchtxt/JSON, errors, corpus) | 4 |
| Cheats (parse, decode + conditionals + run, malformed, corpus of file and word mutations) | 4 |
| config.json, shader, mod.json | 2 + 1 + 2 |
| `tests/mods.rs` (golden patch application on the synthetic module with a pinned SHA-256, selection gating, IPS per module, overlap/header/crossing misses, path-traversal and symlink packages rejected, hash-manifest tampering, policy errors/warnings, romfs overlay, cheat gating, 300-case corrupted-package fuzz, deterministic pack, committed fixture check, CLI round trip) | 14 |

Parser corpus tests mutate valid input thousands of times (random byte changes and truncations) and require `Result`, never a panic.

Workspace gates (Linux): `cargo test --workspace` green, `cargo clippy --workspace --all-targets -- -D warnings` clean, `cargo fmt --check` clean, `python3 -I tools/check_docs.py` OK (123 files), `mkdocs build --strict -f docs-site/mkdocs.yml` OK (mkdocs-material from `docs-site/requirements.txt` in a scratch venv).

## The CLI on the example mod
```
$ napple mods validate docs/mods/examples/synthetic-fix
docs/mods/examples/synthetic-fix: 0 error(s), 0 warning(s)
$ napple mods pack docs/mods/examples/synthetic-fix -o synthetic-fix.napmod
packed synthetic-fix.napmod (sha256 834536a023b96eb4a3fabb9f2c5e46edceba7854414135a509f724cd36fcdc0b)
$ napple mods inspect synthetic-fix.napmod
Synthetic Fix (example) 1.0.0 (example.synthetic-fix)
title      0100E4A100000000   builds 0123456789ABCDEF0123456789ABCDEF
author     Napple docs   license CC0-1.0
signature  none (unsigned: only enable mods from people you trust)
package    sha256 834536a023b96eb4a3fabb9f2c5e46edceba7854414135a509f724cd36fcdc0b
content    sha256 045ebd21bb9e829f1e31ca096f90ee399c72f9a4c9535157ee301813aff90f9c   hash manifest: ok
contains   code patches: YES (7 records)   cheats: YES (2)   romfs: YES   config: YES   shaders: YES
           code patches change the game's code: they stay off until you enable them for this mod
size       9 files, 2426 bytes

A made-up fix for a made-up title. It shows every component of the .napmod format; it contains no game data and applies to nothing real.
$ napple mods apply synthetic-fix.napmod --image docs/mods/examples/synthetic-module.bin --build-id 0123456789ABCDEF0123456789ABCDEF --layout 0x1000,0x1800 --enable-code --dry-run
module 0123456789ABCDEF0123456789ABCDEF00000000000000000000000000000000  (dry run)
  hit   0x00000140 (+0x40)     4 B Text  example.synthetic-fix@1.0.0:patches/fix.pchtxt#Skip intro check [docs]
  hit   0x00000200 (+0x100)     4 B Text  example.synthetic-fix@1.0.0:patches/fix.pchtxt#Skip intro check [docs]
  hit   0x00001110 (+0x1010)     7 B Rodata  example.synthetic-fix@1.0.0:patches/fix.pchtxt#Rename the title string [docs]
  MISS  0x00005100     4 B OutOfRange  example.synthetic-fix@1.0.0:patches/fix.pchtxt#Address beyond the module (shows a MISS in the apply report)
  skip  example.synthetic-fix@1.0.0:patches/FEDCBA9876543210FEDCBA9876543210.ips: build id FEDCBA9876543210FEDCBA987654321000000000000000000000000000000000 is another module
  skip  example.synthetic-fix@1.0.0:patches/fix.pchtxt#Debug flag on [docs]: @disabled by default
3 hit(s), 1 miss(es), 0 overlap(s), 2 skipped, 15 byte(s) would change
sha256 8b8e051f30bf577763604c4a8c360f0468eb3479258170132c8cfd2e1fc9ff27 -> 49837786c2137309502568c4f92e9387c3d63b16265413438f6dd15aa45e3600
```
The MISS is intentional (a record far beyond the module) and the `.ips` for another module is skipped.

## Not done / Mac follow-ups
1. `napple build --mods`: call the hook in `pipeline.rs::rewrite_version` (call site in `docs/mods/integration.md`), key data dirs and caches by the applied mod set, record applied mods in `manifest.json`.
2. Runtime romfs overlay lookup in `runtime/src/services/romfs_map.c` (suggested: build writes `mods/overlay.json`, runtime reads it).
3. Cheat execution against guest memory, per-title config application, shader replacement hook in gfx.
4. Studio / overlay UI (`inspect --json` and `validate --json` are the interface).
5. Real signatures (field reserved, digest = the hash manifest).
6. Not verified on real games: no game, firmware or macOS here; offsets were verified only against a synthetic module. Check on a Mac: a real pchtxt fix from a known build (for example one for Super Mario Odyssey) applied in `--dry-run` on the dumped `main`, then a build with the hook.
7. Cheat opcodes beyond the subset (loops, registers, key conditions, arithmetic).
