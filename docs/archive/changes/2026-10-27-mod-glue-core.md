# mod-glue-core (2026-10-27, Linux cloud): `napple build --mods`, romfs overlay lookup, control.nacp

Branch `claude/mod-glue-core-2026-10-27`. QoL filler (0 coverage points). Status words: tested = unit/e2e test passes on Linux; nothing here was run on a Mac.

## What changed
- `crates/napple-mods/src/buildglue.rs` (new): `ModSet` (load/validate/resolve, set key, manifest record, apply, overlay writer).
- `crates/napple-build`: `--mods` option (`Opts.mods`); `rewrite_version_mods` applies patches before the AOT rewrite (and before the AArch32 path); `modules/<name>.mods.json`; manifest `mods`/`mod_set`/`mods_overlay`; the cache directory name includes the mod key; `control.rs` keeps the raw NACP (`nacp_raw`) and the pipeline writes `<data dir>/control.nacp` for every version.
- `runtime/src/services/romfs_map.{c,h}`: `romfs_overlay_*` (load, lookup with replace/delete/subtree delete, child listing); host test `runtime/tests/host/test_romfs_overlay.c` (+ Makefile entry).
- `docs/mods/integration.md`: status section.

## Decisions
- Naming a mod in `--mods` is the explicit enable (code patches, cheats, shaders on). Per-block toggles are a Studio feature, not a CLI one.
- The data directory name stays the unpatched build id (the runtime finds it by build id); the applied set lives in manifest.json and in the cache key.
- Patch misses warn (build note), they do not fail the build; a mod for another title or an invalid mod fails.
- The overlay file format is line-oriented JSON so the C reader needs no JSON library; overlay paths with quotes, backslashes or control chars are refused.
- `control.rs` is not on the assignment's owned list but was the only way to get the raw NACP bytes; the change is one additive field.

## Verified / not
- tested: `cargo test --workspace` green, `cargo clippy --workspace --all-targets -- -D warnings` clean, `cargo fmt --check` clean; e2e `crates/napple-build/tests/mods_e2e.rs` (patch lands in `patched.bin` before the rewrite, manifest record, overlay files, control.nacp, wrong-title refusal); `crates/napple-mods/tests/buildglue.rs`; `test_romfs_overlay` (17 checks, ASan/UBSan clean).
- `make -C runtime test-host` does not build on Linux at this base (`threads.c` uses Mach thread state); only the new target was built, `make -C runtime build/host/test_romfs_overlay`.
- Not verified on a Mac: a real title build with `--mods`, the runtime consuming the overlay (fs.c is not wired), ns 400/21/23 reading control.nacp (ipc-depth-rest2).
