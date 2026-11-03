# Mod framework: library API, build hook and runtime

For contributors. The portable core lives in `crates/napple-mods` (no macOS dependencies, `cargo test -p napple-mods` runs on Linux). User-facing rules are in [format](format.md).

## Library map

| Module | What it does |
|---|---|
| `manifest` | `mod.json` parse and field checks |
| `source`, `model` | open a `.napmod` or a folder safely (`LoadedMod::open`/`load`), capabilities, hash manifest, component loading, `Selection` and `ResolvedMod` |
| `ips`, `pchtxt` | parse/write IPS and IPS32, parse/write/convert pchtxt and its JSON form |
| `cheats` | cheat file parser, opcode decoder (`decode`), interpreter (`run` over a `CheatMemory`) |
| `romfs` | `build_overlay(&[&LoadedMod])` -> `OverlayMap` with `lookup("/path")` and `read_entry` |
| `config` | `config.json` parse and `merge` |
| `shader` | `shaders/<hash>.msl` validation |
| `validate` | `validate(&LoadedMod) -> Report` (errors, warnings, capabilities) |
| `pack` | `pack_dir(dir, out)`, deterministic |
| `apply` | the hook below and the apply report |
| `cli` | `napple mods ...`, registered in `napple-cli/src/main.rs` as `Some("mods") => napple_mods::run` |

## The build hook

```rust
pub fn apply_mods_to_module(build_id: &[u8; 32], module: &mut ModuleBytes, mods: &[ResolvedMod]) -> ApplyReport;
pub fn dry_run_mods_for_module(build_id: &[u8; 32], module: &ModuleBytes, mods: &[ResolvedMod]) -> ApplyReport;
```

`ModuleBytes` holds the three decompressed segments with their memory offsets (`ModuleBytes::from_segments([(off, bytes); 3])`). `apply_mods_to_module` applies, in the given order, every patch of every enabled mod whose build id equals `build_id` (`Selection::enabled && code_patches`, plus per-file and per-block overrides, `@disabled` blocks off), never writes outside a segment, never panics on hostile input, and returns hits, misses, overlaps (later wins), skipped patches and the SHA-256 of the module before and after. Truncation is never applied.

### Where the build calls it

`napple build --mods <a.napmod,dir,...>` (comma list, the option may repeat; `--mods-strict` makes a missed patch record fail the build).

* `napple_mods::buildglue::ModSet` loads and validates every path (a validation error or a mod for another title fails the build, the same id twice fails), resolves with `enabled + code_patches + cheats + shaders` (naming a mod on the command line is the explicit enable), and exposes `key()`, `apply()`, `manifest_json()` and `write_overlay()`.
* `crates/napple-build/src/pipeline.rs` `rewrite_version_mods` applies the patches right after `Nso::parse` and before the AArch32 check and the AOT rewrite; per-module reports go to `modules/<name>.mods.json`; misses become build notes. AArch32 modules go through `tables32` instead of the rewriter; the same hook works on their segments.
* The data directory keeps its unpatched build-id name (`<buildid16>`; the launcher and runtime find it by that name). The applied set is in `manifest.json`: `mod_set` (hash of ids, versions, content hashes and selections), `mods` (id, version, content hash, per-module hits and misses), and `mods_overlay`, `mods_cheats`, `mods_config` when present. The RomFS stream cache is keyed by the mod set, so a different set never reuses another set's cache.
* Every build rewrites the whole `<data dir>/mods` directory (an empty set clears it) and removes stale `modules/*.mods.json`, so a rebuild into the same output folder never keeps an earlier set's overlay, cheats, config or reports. Files: `mods/overlay.json` (one entry per line: `{"path":"/a/b","file":"mods/files/<n>/a/b","size":N}` or `{"delete":"/a/b"}`) plus the copied files below `mods/files/<n>/`, `mods/cheats.txt`, `mods/config.json`. Paths with quotes, backslashes or control characters are refused at build time.

Caveats of the hook:

* `Nso::parse` checks segment hashes before the hook runs, so patched segments are not re-verified: correct.
* The tables (`tables::build_tables`) are read from the original NSO bytes, so patches that touch `.dynsym`, `.dynamic` or relocation data are not seen by them. Do not patch those areas.

## Runtime side

* **RomFS overlay.** The runtime builds a *virtual RomFS image*: `runtime/src/services/romfs_map.c` `romfs_merge_open` reads the original image through whatever backend serves it (`romfs.bin`, `romfs.map`, delta layer), parses the directory and file tables into a tree, applies `mods/overlay.json` (deletes first, then replacements and additions, so a replacement under a deleted subtree wins, as in `romfs_overlay_lookup`) and generates a fresh header, directory/file tables and hash tables in memory. File data is never copied: every file is an extent of the original image or of a mod file (fds opened on demand, 8 cached). `fs.c` `storage_with_overlay` swaps it in for `OpenDataStorageByCurrentProcess`; any failure serves the original image and logs why. The builder is not involved: re-packing multi-GB images per mod set would need its own cache keys, while the runtime variant costs memory only for the tables and works for all three image backends. Log line: `mods: romfs overlay applied: N replaced, N added, N removed, N skipped`. DLC images (`OpenDataStorageByDataId`) are not overlaid.
* **Cheats** (see [format](format.md#cheat-opcode-table)): Rust and C interpreters with the same opcode table, loops, registers, pointer chains, conditions; `mods_runtime.c` loads `mods/cheats.txt` and `mods_cheats.c` runs them once per frame from `vi.c`'s vsync thread, over a memory view of the guest address space (main module base, heap, alias, aslr).
* **Config**: `mods_config_apply_settings` maps `renderScale`, `aspect`, `fpsMode` onto a *copy* of the frontend settings in `app.m` `push_gfx_settings` (the user's `settings.json` is never rewritten); `fixes` are read with `mods_fix_enabled(name, default)` by title-specific code.
* `NAPPLE_NO_MODS=1` turns the overlay, cheats and config off for a run.

## Open work

1. **Shaders**: the gfx shader cache should look up `shaders/<hash>.msl` by program hash before translating; compile errors must fall back to the translated shader and log once.
2. **UI**: Studio and the overlay list installed mods with author, license, hashes, capabilities and warnings from `inspect --json`, the four switches (enabled, code patches, cheats, shaders), per-block toggles, mod order, and the overlay conflicts. Code patches need an explicit second confirmation. `napple mods inspect --json` and `validate --json` are the machine interface. A Studio field for `--mods` is part of this.
3. **Signatures**: a later format version can fill `signature` and verify it against a user-managed key list; the hash manifest already gives the signed digest.
4. **Data directory name**: keying the directory name itself by the mod set (for example `<buildid16>+<modhash8>`) would need the launcher and runtime lookup changed; today one output folder holds one mod set at a time.

## Tests

`cargo test -p napple-mods`: unit tests per parser (IPS, pchtxt, cheats, config, shader, manifest), corpus tests that mutate valid input thousands of times and must never panic, and `tests/mods.rs`: golden patch application on the synthetic module (`examples/synthetic-module.bin`, regenerated and compared by a test; `NAPMOD_BLESS=1` rewrites it), path-traversal and symlink packages rejected, hash-manifest tampering, policy checks, overlay ordering, selection gating, corrupted-package fuzz, and the CLI round trip.
