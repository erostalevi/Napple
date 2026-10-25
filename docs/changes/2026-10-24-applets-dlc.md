# applets-dlc (batch 2026-10-24-plan5): DLC mounting plus error, player select and controller applets

Branch `claude/applets-dlc-2026-10-24-plan5`, Linux cloud. Status: **tested** (Linux mock), **unbuilt** on macOS.

## Changed
- `aoc:u` (misc.c, new aoc_core.[ch]): count/list/base id/prepare from the manifest's `dlc` array; no DLC keeps today's answers.
- `fsp-srv` 30 `OpenDataStorageByDataId` (fs.c, small append; weak no-DLC default so the host-only fs tests still link): serves a DLC's romfs.bin read-only.
- `applet.c` + new `applet_lib_core.[ch]`: error (0xe) and player select (0x10) library applets dispatched; controller (0xc) answer moved to the core and now honours single mode.
- Tests: `runtime/tests/test_svc_aoc.c` (45 checks), `test_svc_applet_libs.c` (70 checks).
- Rust: no change needed. `napple-build` already embeds selected DLC (`dlc/<id>/v<ver>/romfs.bin`, manifest `dlc` entries) and `napple-core` parses CNMT AddOnContent; `cargo test -p napple-build -p napple-core` passes.

## Verified vs not
- tested: all new logic, 26 Linux test binaries (ASan+UBSan), `check-syntax.sh` clean, cargo tests.
- unbuilt/untested: everything on macOS, fs.c command 30 (no host test written), a real game with DLC.
- [unverified] against a console/title: aoc command ids/layouts, `R_AOC_NOT_FOUND`, libnx struct layouts.
