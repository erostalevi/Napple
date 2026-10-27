# Mod framework: library API, build hook and follow-ups

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

### Exact call site proposal for `napple-build` (a Mac follow-up; `crates/napple-build/src` was read only in this batch)

`crates/napple-build/src/pipeline.rs`, `rewrite_version`, inside the `par_iter` closure, right after `let nso = Nso::parse(bytes)...` and before the AArch32 check and `napple_rewrite::rewrite_module`:

```rust
let mut nso = Nso::parse(bytes).map_err(...)?;                   // was: let nso
if !mods.is_empty() {                                            // new parameter: &[napple_mods::ResolvedMod]
    let mut seg = napple_mods::ModuleBytes::from_segments([
        (nso.text.mem_offset,   std::mem::take(&mut nso.text.data)),
        (nso.rodata.mem_offset, std::mem::take(&mut nso.rodata.data)),
        (nso.data.mem_offset,   std::mem::take(&mut nso.data.data)),
    ]);
    let rep = napple_mods::apply_mods_to_module(&nso.build_id, &mut seg, mods);
    [nso.text.data, nso.rodata.data, nso.data.data] = seg.into_segments();
    progress("mods", ..., &format!("{label} {name}: {} hit(s), {} miss(es)", rep.hits.len(), rep.misses.len()));
    reports.lock().push((name.clone(), rep));                    // written to modules/<name>.mods.json
}
```

Caveats for the integrator:

* `Nso::parse` checks segment hashes before the hook runs, so patched segments are not re-verified: correct.
* `tables::build_tables` receives `nso: Some(bytes)` (the original file) and reads symbols and relocations from it. Patches that touch `.dynsym`, `.dynamic` or relocation data would be missed; refuse such patches in the hook (module offsets inside the `DT_*` tables) or pass the patched segments through.
* The runtime data directory is named by the *unpatched* `main` build id. A patched build needs its own directory (for example `<buildid16>+<modhash8>`) and `manifest.json` should record the mod ids, versions and content hashes that were applied, so a rebuild with a different mod set never reuses a stale `patched.bin`. Build caches must include the content hashes of the mods in their key.
* A miss in a *requested* patch should fail the build when the user asked for strict mode, and warn otherwise.
* AArch32 modules (MK8D) go through `tables32` instead of the rewriter; the same hook works on their segments (offsets are file offsets as well).

## Status of the build glue (2026-10-27, mod-glue-core)

Done (tested on Linux, not verified on a Mac): `napple build --mods <a.napmod,dir,...>` (comma list, option may repeat).

* `napple_mods::buildglue::ModSet` loads and validates every path (a validation error or a mod for another title fails the build, id twice fails), resolves with `enabled + code_patches + cheats + shaders` (naming a mod on the command line is the explicit enable), and exposes `key()`, `apply()`, `manifest_json()` and `write_overlay()`.
* `pipeline.rs` `rewrite_version_mods` applies the patches right after `Nso::parse` and before the AArch32 check and the AOT rewrite; per module reports go to `modules/<name>.mods.json`; misses become build notes. Tables are still read from the original NSO bytes (see the caveat above: patches on `.dynsym`/`.dynamic`/relocation data are not seen by the tables).
* The data directory keeps its unpatched name (`<buildid16>`); the applied set is in `manifest.json` (`mod_set` = hash of ids, versions, content hashes and selections; `mods` = id, version, content hash, per-module hits and misses; `mods_overlay` when a romfs overlay exists). The RomFS stream cache directory name includes the mod key, so a different mod set never reuses another set's cache. Building a different mod set into the same output folder rewrites that data directory.
* `<data dir>/mods/overlay.json` (one entry per line: `{"path":"/a/b","file":"mods/files/<n>/a/b","size":N}` or `{"delete":"/a/b"}`) plus the copied files below `mods/files/<n>/`. Paths with quotes, backslashes or control characters are refused at build time.
* Runtime: `romfs_overlay_load/lookup/child/free` in `runtime/src/services/romfs_map.c` (portable C; `make -C runtime build/host/test_romfs_overlay`). The lookup is not yet called from `fs.c` (the RomFS is served as a raw IStorage image, so a path-level overlay needs a romfs directory/file reader in front of it, or the builder re-packing the image): open item 2 below.
* `napple build` also writes `<data dir>/control.nacp` (the raw NACP of the newest version with a control NCA) for ns 400/21/23.

## Mac follow-up list

1. **Build integration** (done for the CLI, see above; Studio field still open): add a `--mods <file.napmod|dir>...` option (and a Studio field) to `napple build`; load with `LoadedMod::load`, `validate`, `resolve(Selection)`, call the hook as above, store the reports, and key the data directory and caches by the applied mod set.
2. **Runtime romfs overlay** (data file and lookup done; the fs.c hook is open):  `runtime/src/services/romfs_map.c` (fsp-srv romfs lookup) should resolve paths through an `OverlayMap`. Suggested shape: the build step writes `<datadir>/mods/overlay.json` (`romfs path -> file inside the app bundle`, plus tombstones) and the runtime consults it before the RomFS image for `OpenFile`, `GetEntryType`, directory listings (merge and hide). Keeping this a data file keeps the runtime free of zip code.
3. **Cheats**: the runtime supplies a `CheatMemory` over the guest address space (main module base, heap, alias, aslr) and runs `cheats::run` or a C port at a safe point once per frame; a C port needs the same opcode table (see [format](format.md#cheat-opcode-subset)).
4. **Config**: apply `merge`d `ModConfig` as per-title settings (render scale in gfx, aspect, fps mode, fix toggles named by the title page).
5. **Shaders**: the gfx shader cache looks up `shaders/<hash>.msl` by program hash before translating; compile errors must fall back to the translated shader and log once.
6. **UI**: Studio and the overlay list installed mods with author, license, hashes, capabilities and warnings from `inspect --json`, the four switches (enabled, code patches, cheats, shaders), per-block toggles, mod order, and the overlay conflicts. Code patches need an explicit second confirmation. `napple mods inspect --json` and `validate --json` are the machine interface.
7. **Signatures**: a later format version can fill `signature` and verify it against a user-managed key list; the hash manifest already gives the signed digest.

## Tests

`cargo test -p napple-mods`: unit tests per parser (IPS, pchtxt, cheats, config, shader, manifest), corpus tests that mutate valid input thousands of times and must never panic, and `tests/mods.rs`: golden patch application on the synthetic module (`examples/synthetic-module.bin`, regenerated and compared by a test; `NAPMOD_BLESS=1` rewrites it), path-traversal and symlink packages rejected, hash-manifest tampering, policy checks, overlay ordering, selection gating, corrupted-package fuzz, and the CLI round trip.
