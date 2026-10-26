# ac-boot-crash (plan8): Animal Crossing v0 SIGSEGV at main+0x19a6250 explained and fixed

## Root cause
The crash is not a rewrite or relocation bug. AC's engine start-up (`main+0x1976db0`, called from nnMain at `main+0x1c713a4`) creates the NVN wrapper singleton
(`str x23,[G]` at `main+0x1977030`, G = `main+0x3a73750`) only after it checks the NVN API version:
`nvnDeviceGetInteger(0) == 0x35 (53)` and `nvnDeviceGetInteger(1) >= 0x138 (312)`, else it returns silently (`main+0x1976f98 b.ne` / `0x1976fa8 b.lt` -> epilogue `0x197765c`).
Our gfx reports the default 55.15, so the check failed, the singleton stayed NULL, and the next virtual call (`main+0x1979b90`) loaded it and did `ldr x1,[x0,#0x20]` with x0 = 0 (fault 0x20).

`runtime/src/main.c` already had `detect_nvn_version()` for exactly this pattern, but it failed twice for AC:
1. its minor window was 5..40 (AC's is 312);
2. it runs after `gfx_init`, and gfx copied the version at init, so even a detected version never reached `nvnDeviceGetInteger`.

## Fix
- `main.c`: minor window 5..1000; after a detection call `gfx_set_nvn_version()`.
- New `napple_gfx_set_nvn_version()` (gfx/src/api_device.c, header, adapter `gfx_set_nvn_version`, stub, runtime/src/gfx.h).
- Log: `main: NVN API version 53.312 detected in the main module`.

## Verified (Mac, arm64, headless, default save root)
- AC v0 (`7fc1baff976aeca4`, built from Testing Material with `napple build --data-only`): before: SIGSEGV exit 139 at 0.34 s; after: gets past the crash, **next blocker `FAIL: IPC command fsp-srv:202 not implemented` at 0.64 s (exit 70)**. fsp-srv 202 belongs to services/fs.c (not this item). Log: `ac-boot-v0-after-fix.log` in the shared notes folder.
- LAS and SMO 45 s each, run concurrently: exit 0, 0 CRASH/FAIL lines, no detection line (their default 55.15 path unchanged).
- `make -C runtime test`: test_media 56, test_ipc 27, fs_journal 16, hid_service 10, timescale 21, 0 failed.
- `cargo test`: not run, no cargo on this Mac (no Rust file changed).

## Notes
- The worktree had 33 untracked " 2.*" File Provider duplicates (e.g. `vt_h264 2.c`) that break `make` (wildcard + spaces); moved out of the tree, not committed.
- Other AC versions (3.0.1/3.0.3) stop earlier on TIPC (tipc-dispatch item); they were not re-run here.
