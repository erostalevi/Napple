# mods-runtime (b7, 2026-10-27/28, Linux cloud): mods in the runtime

Status words: *tested* = unit test passes on Linux; *unbuilt* = never compiled on macOS. Nothing here is *verified* on a Mac.

## What changed

1. **Path-level RomFS overlay.** `fs.c` serves the RomFS as a raw IStorage image, so a lookup cannot work: the guest parses the tables itself. `romfs_map.c` now has `romfs_merge_open`, which reads the original image through its backend (romfs.bin, romfs.map or delta layer), parses the directory/file tables into a tree, applies `mods/overlay.json` (deletes first, then replacements and additions) and builds a virtual image with new header, tables and hash tables. File data is an extent of the original or of a mod file; nothing is copied. `fs.c` `storage_with_overlay` swaps it in; any failure serves the original image and logs the reason. Decision recorded in `docs/mods/integration.md` (why runtime, not re-pack).
2. **`napple build --mods` wiring completed.** The build rewrites `<data dir>/mods` on every build (an empty mod set clears it) and drops stale `modules/*.mods.json`; it writes `mods/cheats.txt` (main build id) and `mods/config.json` (merged); `manifest.json` gains `mods_cheats` / `mods_config`; `--mods-strict` fails on a missed patch record.
3. **Cheats.** The Rust opcode table grew from store/conditional to offset registers, loops, immediate and memory register loads, pointer chains, register stores with increment/offset, and legacy arithmetic, with a step budget. A C twin (`runtime/src/mods_cheats.c`) is tested against the same hand cases and a shared fixture (`docs/mods/examples/cheat-vectors.txt`). `mods_runtime.c` runs it once per frame from the vsync thread (`vi.c`), over the guest address space; writes below the main module's data segment are refused.
4. **Config.** `mods_config_apply_settings` maps `renderScale`, `aspect`, `fpsMode` onto a copy of the frontend settings in `app.m` `push_gfx_settings`; `settings.json` is never rewritten. `mods_fix_enabled` serves the `fixes` map.

## Verified vs not

| Item | Status |
|---|---|
| merged RomFS (replace, add, delete, directory listing, hash lookup, hostile original, ASAN/UBSAN) | tested (`runtime/tests/host/test_romfs_merge.c`) |
| `fs.c` wiring | unbuilt (object compiles on Linux with the mach shim; `fs` tests do not link on Linux) |
| C cheat engine | tested (`test_mods_cheats.c`, shared fixture) |
| `mods_runtime.c` (files, guest memory view, config mapping) | tested on host stubs (`test_mods_runtime.c`) |
| `app.m` / `vi.c` hooks | unbuilt (`vi.c` syntax-checked on Linux; `app.m` is Objective-C) |
| builder: cheats/config/stale clearing/strict | tested (`cargo test -p napple-build --test mods_e2e`) |
| real game: pchtxt fix, romfs replacement, headless boot | **not done** (needs a Mac) |

## Files

`runtime/src/services/{romfs_map.c,romfs_map.h,fs.c,vi.c}`, `runtime/src/{mods_cheats.[ch],mods_runtime.[ch],app.m}`, `runtime/Makefile`, `runtime/tests/host/test_{romfs_merge,mods_cheats,mods_runtime}.c`, `crates/napple-mods/src/{cheats,buildglue}.rs`, `crates/napple-build/src/{cli,pipeline}.rs`, `crates/napple-build/tests/mods_e2e.rs`, `docs/mods/**`.

## Not done

* Data directory keyed by mod set (launcher/runtime find it by build id; changing that is outside this area). Stale-file clearing makes a rebuild safe instead.
* Shader replacement and the Studio/overlay UI (backlog, not this area).
* Cheat opcodes 8 (key press), 9, A, C0..C2, FFF: unsupported on purpose (the cheat is skipped whole).
