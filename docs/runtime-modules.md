# Runtime data directory and module tables

```
work/games/<titleid>/<buildid16>/
  manifest.json      {format, title_id, build_id (first 16 hex of the main ModuleId), load_order[], modules[{name,file_size,sha256,build_id,...}]}
  main.npdm          copied from the ExeFS (service access list, svc mask, main thread)
  modules/<name>.patched.bin   rewritten image (rewriter output: image + key page + patch area)
  modules/<name>.tables        NTB1 tables (below); the rewriter's .gates.tsv/.fixups.tsv/.sites.tsv stay alongside as evidence
  romfs.bin          decrypted RomFS image (IVFC data level), served raw by fsp-srv
```
Load order: `main`, `subsdk0..N`, `sdk` (symbols resolve in that order). Nothing per game is compiled into the runtime.

NTB1 (little endian): `"NTB1"`, u32 version=1, u32 nsym,nrel,ngate,strtab_size; u64 file_size,text_end,ro_off,data_off,patch_off,key_off;
u64 dt_init,dt_fini,init_array,init_array_size,fini_array,fini_array_size; u8 build_id[32]; then `sym[nsym]{u32 name_off,u8 binding,type,defined,pad,u64 value,size}`,
`rel[nrel]{u64 where,i64 addend,u32 kind,u32 symbol}` (kinds 257/1025/1026/1027; RELR is expanded to RELATIVE),
`gate[ngate]{u64 ret,u64 original,u32 timer,u32 arg}` and the string table (`timer`: 0 svc, 1 cntpct/cntvct, 2 cntfrq). Now written in process by
`napple build` (`crates/napple-build/src/tables.rs`, a byte-exact Rust port of `tools/prepare_tables.py`: identical bytes on Super Mario
Odyssey and Link's Awakening, plus a golden test against the Python output). The patched image/`.gates.tsv`/`.fixups.tsv` formats are the rewriter's.

Pipeline for development: `napple build <game files> -k <prod.keys> --data-only -o work/games` (no Python, replaces `tools/prepare_game.sh`: writes
`work/games/<titleid>/<buildid16>/` for the base and every selected update, plus `dlc/`, `system/`, `napple-app.json`; add `--updates none` for the base only).
Legacy pipeline: `tools/prepare_game.sh <nsp|xci> <prod.keys> [--romfs]` (ExeFS dump via `napple build --dump-exefs`, the Rust
rewriter `napple rewrite` (docs/rewrite-format.md; `REWRITER=python` selects the reference stopgap), tables, npdm). Both games boot identically with either rewriter. `napple build --dump-romfs` streams
`romfs.bin`. Keep one game's `romfs.bin` on disk at a time. `tools/make_empty_romfs.py` makes a valid empty image for tests.
Local change to the rewriter: `ldrsw_table_scan` (jump tables whose bound is not a plain cmp/b.hi), needed by `subsdk0` of both games.

Entry: the runtime calls `nn::init::Start(mainThreadHandle, 0, cb1, cb2)` (or the 3-callback SDK 15 form): `cb1` runs the `sdk`
module's DT_INIT after nn::os is up, `cb2` runs every other module's DT_INIT before `nnMain`.

## App bundle layout and launch contract (`napple build ... -o <outdir>` -> `<outdir>/<Name>.app`)

```
<Name>.app/Contents/
  Info.plist  PkgInfo  _CodeSignature/  Resources/AppIcon.icns
  MacOS/napple-launcher      packaging/launcher.c: no-argument entry point, execs the sibling runtime
  MacOS/napple-runtime       the runtime binary (copied from runtime/build/napple-runtime or --runtime)
  Resources/game/
    napple-app.json          {format, title_id, name, display_version, versions[], dlc[], defaults{version,dlc[]}, system{fonts,firmware_version}, notes[]}
    default                  one line: directory name of the default version (the newest embedded one)
    <buildid16>/             one runtime data dir per version (base and every update): the layout above + manifest.json extras
    dlc/<titleid16>/v<ver>/romfs.bin    one layer file per DLC pack (decrypted RomFS data level)
    system/fonts/            shared fonts extracted from the user's firmware (see below)
```
**Launch contract.** Finder / `open` start `Contents/MacOS/napple-launcher` with no arguments. It runs
`napple-runtime <Contents/Resources/game/<version dir>> [args...]` where the version dir is `$NAPPLE_VERSION` if set, else the first line of
`game/default`. Any argument is passed through (`--headless --seconds 20`, `--save-root DIR`...), Finder's `-psn_*` is dropped, and
`NAPPLE_BUNDLE_DIR=<Contents>` is exported. So the runtime keeps its single-argument contract (`<data-dir>`); it needs no bundle-relative logic.
The runtime may later read `<data-dir>/../system`, `../dlc/...` itself (paths are in each `manifest.json`).

`manifest.json` of a version dir keeps the keys the runtime reads (`format,title_id,build_id,load_order,modules[]`) and adds, ignored today:
`kind` (base|update), `version`, `label`, `romfs {mode: full|patched|delta|base, file, size, sha256}`, `system_dir` (`../system`) and
`dlc[]` `{title_id, version, label, pack_size, romfs_size, sha256, dir ("../dlc/<id>/v<ver>"), romfs ("romfs.bin"), default_enabled}`.
`title_id` is the application (base) id for every version; `build_id` is the first 16 hex of that version's own `main` ModuleId, which is also
its directory name. Each version has its own `modules/` (own rewrite and tables), `main.npdm` and RomFS.

**Update RomFS.** Default (`--update-romfs materialize`): the BKTR-patched RomFS is streamed to `<update dir>/romfs.bin` (a full image, boots
with today's runtime; costs one RomFS of disk per update). `--update-romfs delta`: `<update dir>/romfs.delta/{romfs.json, romfs.reloc, patch.bin}`
and no `romfs.bin`; `romfs.reloc` = `"RLC1", u32 count, u64 data_size`, then `count` x `{u64 data_offset, u64 source_offset, u32 source, u32 0}` where
entry i covers `[data_offset_i, data_offset_{i+1})` (the last to `data_size`) of the patched RomFS data level, `source` 0 = `<base dir>/romfs.bin`
(`base_dir` in `romfs.json`) and 1 = `patch.bin`. An update without a RomFS of its own gets a clone of the base `romfs.bin`. No runtime support for
the delta layer exists yet (the runtime would compose `read(off)` through the table); BKTR is unvalidated against retail (see docs/changes/2026-10-22-packaging.md).

**Shared fonts (pl:u).** `system/fonts/<firmware file name>.{bfttf,ttf}` + `fonts.json` (`type` = `nn::pl::SharedFontType` 0..5, `name`, sizes, sha256,
source title id; BFTTF = `u32 0x18029A7F^K, u32 BE size^K`, TTF XOR K with K=0x06186249). `napple build --dump-fonts <firmware.zip|dir> -k <prod.keys> -o <dir>`
writes the same files straight into `<dir>` (what `runtime/src/services/pl.c` reads from `<save-root>/system/fonts`). Because that service reads
the save root, the launcher copies/clones the bundle's `.ttf` files into `<save-root>/system/fonts` when missing or of a different size; a runtime
fallback to `<data-dir>/../system/fonts` would make this unnecessary. Without `--firmware` the build succeeds and records a `compat:` note in `napple-app.json`.

**Run-time modules (NROs).** `napple build` also rewrites every `*.nro` file of each version's RomFS into `<version dir>/nro/<module id
hex, 64 chars>.{patched.bin,tables}`. The tables are NTB1 with the module id as build id, and the dynsym extent comes from the NRO header
(0x78/0x7C) for GNU-hash modules. These are listed in `manifest.json` under `nro` as `[{path, module_id, nro_sha256, file_size, gates,
relocs}]`. `ldr:ro` (`runtime/src/ro.c`) maps the image whose module id matches the NRO the game passes. A RomFS `.nro` that is not a
valid NRO (or cannot be rewritten) is listed in the app's notes instead. The rewriter treats an NRO header (0x10..0x80 of text, "NRO0" at
0x10) as data.
