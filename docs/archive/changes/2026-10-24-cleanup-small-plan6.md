# Cleanup of plan5 follow-ups (branch claude/cleanup-small-2026-10-24-plan6)

Status: all *verified* on an Apple M5 Mac (`make -C runtime test test-services`, `make -C gfx test`: 0 failures).

* `hz_result.h`: `HZ_MOD_AUDIO` (153; `HZ_MOD_AUDIO_IN` is now an alias) and `HZ_R_AUDREC_BUFFER_COUNT_LIMIT` / `HZ_R_AUDREC_BUFFER_NOT_FOUND`; `audrec.c` uses them. Values unchanged (still [unverified]). `audin.c` is not mine and keeps `HZ_MOD_AUDIO_IN`.
* `scan.py`: the NVN regexes accept `NVN_NOPROTO(`. The scan reports NVN 523/523 implemented, 0 missing (no snapshot saved). The earlier "101 missing" was already gone because those entries moved to `nvn_protos_ext.h` and use plain `NVN(`; the regex change is future-proofing.
* `docs/runtime-services.md`: `aoc:u` table row updated (DLC is mounted via fsp-srv cmd 30).
* New `runtime/tests/test_svc_fs_dataid.c` (12 checks): cmd 30 opens a DLC romfs storage (GetSize, Read), missing romfs / unknown id / app id fail with NOT_FOUND_DATA.
* `gfx/tests/t_getters_linux.sh` delegates to the Makefile target on macOS. `t_getters_linux.c` was already built and run by `make -C gfx test` (33 checks pass), so nothing to add there.
* `maxwell_translate.c`: F2F F16 scratch buffer `h` 1100 -> 2200 bytes so gcc's `-Wformat-truncation` cannot trigger (clang never warned). `hid.c`: removed unused `ret_u32_0`, `ret_u8_0`.
