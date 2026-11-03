---
branch: claude/nvjpg-headerless-redo-2026-11-03-r11
orchestration: nvjpg-headerless
base: 35fa084
head: e8d7ba7 (note commit follows)
env: mac
date: 2026-11-02
areas: [runtime/services, docs]
status: integrated
---

# claude/nvjpg-headerless-redo-2026-11-03-r11: NVJPG header-less decode on the real nvjpg_drv_pic_param_s layout

## Summary
Landed the two library commits of the cloud branch (420a1a1 decode_frame split, cff3758 `napjpeg_decode_raw_rgba` + `test_svc_jpeg_raw`; the one jpeg_core.h conflict kept both
halves) and rebuilt the engine path on the real layout: a decode whose bitstream buffer has no SOI and whose `dec_frame_size` is programmed now decodes from the pic
struct's decoder half (frame size, stream chroma mode, restart interval, `dec_block_ctl`, `dec_quant`, Huffman mincode/symbol) plus the entropy segment, and writes
the same surface as the headered path (shared output stage). No register added, the old reduced model (0x101..0x109, table block) is not back. The headered path is unchanged.
The branch is cut from main (35fa084), NOT from the worktree's starting commit f1206c1 (that is the published squash lineage and does not merge with main), then main merged again at hand-off (no runtime/ changes in that merge).

## Files
runtime/src/services/nvdrv_media_core.c (nvjpg_huff, nvjpg_raw_params, nvjpg_decode branch), jpeg_core.[ch], tests/jpeg_strip.h, test_svc_jpeg_raw.c, test_svc_nvdrv_media.c (+22 checks),
docs/reference/nvdrv-layouts.md, docs/architecture/runtime-services.md, docs/archive/changes/2026-11-03-nvjpg-headerless-redo-2026-11-03-r11.md (+ README row).

## Built and tested here (macOS, GFX=napple)
| Item | Status | Evidence (before = main 35fa084 -> after) |
|---|---|---|
| test_svc_nvdrv_media | verified | 293 -> 315 checks, 0 failed |
| test_svc_jpeg_raw (new) | verified | 64 checks, 0 failed; test_svc_jpeg 43/0, test_svc_caps_e2e 22/0 |
| make test, test-services, test-ipc-reference | verified | rc 0, no "missing N>0" |
| make test-risk-title | verified | 158 checks, 0 failed, 0 skipped |
| cargo test --workspace --locked | verified | 449 passed, 0 failed |
| 60 s boots (ACNH 90 s), NAPPLE_NET unset and =host: SMO 0100000000010000, MK8D 0100152000022000 (newest build), 01006BB00C6F0000, ACNH 01006F8002326000 | verified | all 8: rc 0, missing_ipc [], unsupported_svcs [], crashes [] |
| MK8D NVDEC/VIC preview (build b5c39b5b..., menu route NAPPLE_INPUT_SCRIPT=15:A,19:A,23:A,27:A,31:A,35:A, 110 s, NAPPLE_MEDIA_TRACE=1) | verified | main binary 16:30 build vs this branch: both class 0xf0 120 executes / 119 frames / 0 errors, class 0x5d 120 / 119 / 0 |

## Findings / risks
- The table semantics are Napple's reading of fields whose positions open-gpu-doc pins but whose meaning it does not state (Huffman slot numbering, what idx counts, symbol layout DC 0..11 / AC 12..173, quant in zigzag order, 8-bit only). Nothing was traced from a title (none in the corpus uses NVJPG): a real driver could differ, the doc lists each assumption. Layout facts only, no GPL code.
- 4:4:0 (422V) has no vector in the corpus; a slot falls back to the component's own index when its row is empty (speculative leniency).
- orchestrations/nvjpg-headerless.md can be moved to the archive as landed once this merges; the old cloud branch stays as pushed.
- Build note: worktrees need gfx/frontend/input/audio libs built (make -C gfx; make -C gfx adapter; make -C frontend/input/audio) and PATH with ~/.rustup/toolchains/stable-aarch64-apple-darwin/bin before make test-services.

## How to integrate
git merge --no-ff claude/nvjpg-headerless-redo-2026-11-03-r11 (fast to merge: only the files above), then make -C runtime GFX=napple test-services.

## Integration result
2026-11-02, Clean (no `integrate:` fixes, no native ports; the C was left as is). Merged into main on top of 7bc0e36 with no conflicts.
Merged tree: make -C runtime GFX=napple test test-services test-ipc-reference rc 0 (ipc_reference missing 0, 111 fixtures); test_svc_nvdrv_media 315/0, test_svc_jpeg_raw 64/0, test_svc_caps_e2e 22/0; test-risk-title 158/0, 0 skipped;
cargo build/test --workspace --locked 449 passed, 0 failed; eight boots (SMO, MK8D newest build, LAS 60 s, ACNH 90 s; NAPPLE_NET unset and =host; lock, private save root) rc 0, missing_ipc [], unsupported_svcs [], crashes [];
MK8D NVDEC/VIC preview (b5c39b5b, 110 s): main and branch both class 0xf0 120 executes / 119 frames / 0 errors, class 0x5d 120 / 119 / 0; check_docs only the archive-orphan complaints; gen_support --check OK.
Correction to the file: the runtime binary must be rebuilt (`make -C runtime GFX=napple`) in a fresh worktree before boots; the `test-*` targets do not produce it.
