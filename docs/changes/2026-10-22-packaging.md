# Packaging: `napple build` (2026-10-22)

## Objective
`napple build <game files...> -k <prod.keys> [--firmware <zip|dir>] [--updates ...] [--dlc ...] [--target macos] -o <outdir> [--name <name>]` turns a
base game (+ selected updates + DLC) into a locally ad-hoc-signed `<Name>.app` with **no Python at build time**: everything runs in one process on
`napple-core` / `napple-rewrite`. The app runs with no arguments (Finder / `open`). Contract with Studio: docs/studio-protocol.md; with the runtime:
docs/runtime-modules.md ("App bundle layout and launch contract").

## Files
- `crates/napple-build/src/`: `lib.rs` (dispatch), `cli.rs` (arguments), `pipeline.rs` (orchestration), `tables.rs` (NTB1 writer), `bktr.rs` (BKTR RomFS),
  `container.rs` (raw NCA/ticket access), `control.rs` (NACP name/version/icon), `firmware.rs` + `zipread.rs` + `sysdata.rs` (firmware, fonts, version),
  `stream.rs` (RomFS streaming + cache), `bundle.rs` (Info.plist, icns, signing, tool discovery), `check.rs` (`--check`), `util.rs` (progress, cancel,
  disk space, clones), `json.rs`, `error.rs`, `legacy.rs` (the old `--dump-exefs/--dump-romfs`, unchanged).
- `crates/napple-build/tests/`: `e2e.rs`, `cancel.rs`, `support/mod.rs` (synthetic NSP/NCA/NSO/BKTR fixtures), `golden/synth.tables` (Python-produced golden).
- `crates/napple-build/Cargo.toml` (+ `Cargo.lock`): new deps `napple-rewrite`, `aes ctr cipher xts-mode sha2` (already in the workspace), `flate2`
  (rust backend, deflate entries of a firmware zip), `libc`, `rayon`.
- `packaging/launcher.c`, `packaging/Makefile` (`make -C packaging` -> `packaging/build/napple-launcher`).
- Docs: this file, `docs/studio-protocol.md`, `docs/runtime-modules.md`.
No file outside `crates/napple-build`, `packaging`, `docs`, `work` was edited. `runtime/` was only read.

## Flow
```
files -> scan (napple-core) -> BuildPlan -> pick updates/DLC          [ingest 2%]
  -> per version: Program NCA -> ExeFS -> for each main/subsdk*/sdk: rewrite + verify (napple-rewrite) -> NTB1 tables   [rewrite 8-24%]
  -> disk-space check (before any multi-GB write)                      [plan 7%]
  -> RomFS: base data level -> cache file (sha256) -> clonefile into <buildid16>/romfs.bin
            update BKTR section -> patched data level -> romfs.bin (default) or romfs.delta/
            DLC data NCA -> dlc/<id>/v<ver>/romfs.bin                  [romfs/dlc 24-88%]
  -> firmware: system data archives -> shared fonts (+ SystemVersion)  [fonts 90%]
  -> icon (JPEG -> icns via sips/iconutil), Info.plist, PkgInfo, launcher + runtime  [bundle 94%]
  -> xattr -cr, codesign --force --deep -s -, codesign --verify --deep --strict     [sign 99%]
  -> atomic rename staging/<Name>.app -> <outdir>/<Name>.app, RESULT <path>          [done 100%]
```
Staging directory and RomFS cache live in the temp dir when it is on the output volume (clones and the final rename stay cheap and atomic; also
keeps iCloud/File Provider folders such as Documents from re-adding Finder metadata to the bundle before signing and from syncing multi-GB cache files),
otherwise in `-o`. On failure or SIGTERM the staging dir is removed, the app is never half-published, a complete RomFS copy stays for a retry.

## Logic and decisions
- **Tables port.** `tables.rs` follows `tools/prepare_tables.py` field by field (first-tag-wins dynamic table, DT_HASH or NSO-header dynsym extent, RELR then
  RELA then JMPREL, string table with the empty-name quirk) with bounds checks instead of asserts. Verified byte-identical to the Python output on **both**
  real games (main/sdk/subsdk* `.tables`, `.patched.bin`, `main.npdm`, `romfs.bin`, manifest module list) and on a synthetic golden.
- **Versions.** Each included version (base and each selected update) gets its own data dir named by its own `main` build id with its own rewritten
  modules, tables, `main.npdm`, manifest and RomFS; `rtld` is not rewritten (the runtime does not load it). Unhandled sites or failed structural check abort the build.
- **RomFS.** Base and DLC: decrypted IVFC data level streamed in 8 MiB chunks, hashed on the way, cached, then cloned (APFS `clonefile`, plain copy fallback),
  so a 6 GB image is written once. Cache hit rule: complete file + sidecar with matching size (sha256 recorded, full rehash not done by default).
- **BKTR (unvalidated against retail).** `bktr.rs` implements AES-CTR-EX subsection tables and relocation tables from the format description (own small NCA3
  reader, because napple-core keeps the section key private). Assumptions that retail must confirm: (1) subsection entries replace IV bytes 4..8 with their
  generation, counter low 8 bytes = absolute NCA offset >> 4, tables use the plain section counter; (2) bucket trees are read as `NodeSize 0x4000`, entry sets
  after the L1 node (+L2 nodes when >2046 sets), offset nodes ignored; (3) relocation entry = `{u64 virtual, u64 physical, u32 source}`, subsection entry =
  `{u64 offset, u32 0, u32 generation}`; (4) base offsets address the base RomFS *section* (including the IVFC levels); if the result is not a valid RomFS
  the base *data level* interpretation is tried; if both fail the build stops with `[bktr]` and suggests `--updates none`. Only synthetic fixtures exist.
- **Update RomFS storage.** Default is a full patched `romfs.bin` per update (works with the runtime as it is; costs a RomFS of disk per update, Studio can show
  it). `--update-romfs delta` writes a documented delta layer instead (format in docs/runtime-modules.md; the runtime has no reader yet).
- **DLC.** Each AddOn pack's data NCA RomFS becomes `dlc/<titleid>/v<ver>/romfs.bin`; manifests and `napple-app.json` list ids, versions, sizes, sha256 and the default
  (all enabled). The runtime does not mount them yet.
- **Fonts.** The six shared fonts (plus `nintendo_ext2`) are found by content in system archives 0100000000000810..0814 (BFTTF header + decoded TTF magic), written
  as `.bfttf` and decoded `.ttf` with `fonts.json`; firmware version comes from 0100000000000809. `--dump-fonts` serves `runtime/src/services/pl.c`
  (which another engineer wired concurrently to `napple build --dump-fonts`). The launcher installs the bundle's `.ttf` into `<save-root>/system/fonts` because
  `pl.c` reads there. Without `--firmware` the build succeeds and records a `compat:` note.
- **Launcher.** Needed: the runtime has no bundle-relative default. `packaging/launcher.c` (no deps) resolves `Resources/game/<default>` and `exec`s the sibling runtime.
- **Signing.** `codesign --force --deep -s -` without hardened runtime and without entitlements: no JIT is used (rewrite is ahead of time), the runtime only
  maps files and changes page protection as it did unsigned; increased-memory entitlements are iOS-only. Verified with `codesign --verify --deep --strict` before publishing.
- **Disk safety.** Free space of the staging and output volumes is compared with the plan (RomFS bytes not already cached + 3x ExeFS + 256 MB) before writing: `[disk-space]`.
- **Errors/progress.** `error: [code] message`; progress is one monotonic overall percentage.
- **`--data-only`** writes `<outdir>/<titleid>/<buildid16>/` (the layout of `work/games`) without a runtime, launcher or signature: a Python-free `prepare_game.sh`.

## Risks / not done
- BKTR retail validation (above); DLC/update toggling and the delta layer need runtime support (not implemented in `runtime/`).
- `--title-keys` is accepted but not applied (titlekey crypto uses the tickets in the NSP/XCI; `napple-core::Pack` keeps tickets private). A small
  `Pack::with_title_keys` in napple-core would fix it. NSZ is not handled (napple-core).
- Output folders managed by iCloud/File Provider (Desktop, Documents) re-add `com.apple.FinderInfo` to the `.app` after publishing: `codesign --verify` then fails
  on the published copy (the app still launches). The build verifies before publishing and records a note; use an output folder outside those.
- Signature is ad-hoc: Gatekeeper treats the app as locally built, fine for the builder's own machine. iOS/iPadOS targets are not implemented (`[target]`).
- The runtime binary is copied from whatever `runtime/build/napple-runtime` is at build time (use `--make-runtime` to refresh); the launch behaviour
  (Super Mario Odyssey crashes in the guest after `fsp-srv`, Link's Awakening runs) is the runtime's own current state and matches a direct run on `work/games`.
- Cache sidecar trusts size + marker; a corrupted cache file of the right size would be reused (delete `.napple-cache.nosync`).

## Tests
- `cargo test -p napple-build`: 30 unit tests (tables golden vs `tools/prepare_tables.py`, hostile modules, BKTR trees/AES-CTR-EX/indirect storage, zip, fonts, plist,
  progress, clones, resume cache, argument parsing, free-space message), 9 end-to-end tests on synthetic NSPs (base + BKTR update + DLC, `--updates none`, delta
  reconstruction, plain-RomFS update + `.app` structure + atomic replace, resume, error codes without key material, base-offset interpretation B, corrupt BKTR table,
  `--check`) and 1 SIGTERM test (cancel code, no staging left). Regenerate the golden: `NAPPLE_FIXTURE_DIR=<dir> cargo test -p napple-build --lib dump_fixture -- --ignored`
  then `work/venv/bin/python -I tools/prepare_tables.py <dir> --exefs <dir>/exefs --out <dir>/out --title-id 0100000000000001` and copy `out/main.tables` to `tests/golden/synth.tables`.
- Real runs (Testing Material, 22.1.0 keys + firmware): SMO `.app` in ~40 s (5.5 GB RomFS), Link's Awakening in ~45 s; `codesign --verify` passes before publishing;
  `cmp` of modules/tables/npdm/romfs against `work/games/...` identical for both games. Both apps start through `napple-launcher --headless --seconds 15`:
  loader, graphics init, `nn::init::Start`, `fsp-srv` RomFS mount; Link's Awakening ran 643 frames and loaded 6 shared fonts; SMO reaches the same guest crash as a direct
  run on `work/games` (identical log).
- Recommended: run a retail update/DLC through `napple build --updates all --dlc all` and boot the update dir with `tools/boot_test.sh`-style flags; compare against a known-good patched RomFS.

## Commands
```
make -C packaging                       # launcher (and make -C runtime for the runtime)
cargo build --release -p napple-cli
napple build --check -k prod.keys --firmware Firmware.zip
napple build "<game>.nsp" -k prod.keys --firmware Firmware.22.1.0.zip -o work/apps
napple build base.nsp upd.nsp dlc.nsp -k prod.keys --updates all --dlc all --target macos -o work/apps --name "My Game"
napple build "<game>.nsp" -k prod.keys --data-only -o work/games          # runtime data dirs only
"work/apps/<Name>.app/Contents/MacOS/napple-launcher" --headless --seconds 20
```
