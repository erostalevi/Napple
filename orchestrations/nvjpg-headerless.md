---
name: nvjpg-headerless
batch: 2026-11-03-r11
branch: claude/nvjpg-headerless-2026-11-03-r11
base: published squash d0d4be9 (cloud branch as pushed, NOT rebased: merge with git merge, resolve conflicts keeping round-10 code)
env: linux-cloud
status: blocked
---
# nvjpg-headerless (brief in the session prompt; see ROUND11.md)
## Result
Not landed (Heavy, 2026-11-03 integrator run). Branch claude/nvjpg-headerless-2026-11-03-r11 (5 commits over published squash 57431a7) was written against the round-9 reduced NVJPG model (registers 0x101..0x109, Napple's 6/12-word setup struct). Main now has the real NVJPG programming model (nvdrv_media_regs.h: class registers 0x1c0..0x1c7, nvjpg_drv_pic_param_s, decode via the driver's SOI-bearing bitstream, BAD_MAGIC for header-less input), so the engine half (commit 5697308: NVJPG_MODE_DECODE_RAW, table block register 0x105, nvjpg_parse_tables, test_svc_nvdrv_media additions, 64+ lines of conflicts in nvdrv_media_core.c / nvdrv_media_regs.h / test_svc_nvdrv_media.c) cannot be merged mechanically; re-expressing it on the real layout is a design decision (where header-less tables come from: the DEC_HUFF_MINCODE / SYMBOL / QUANT groups of the real pic struct instead of Napple's table block).
Verified here: the library half (420a1a1 decode_frame split, cff3758 napjpeg_decode_raw_rgba + test_svc_jpeg_raw) cherry-picks with one both-appended conflict in jpeg_core.h and builds and passes on macOS (test_svc_jpeg_raw 64 checks, 0 failed); it was left out so the item stays whole.
Next step: a new item "nvjpg-headerless-redo" that lands the two library commits and rebuilds the engine path on the real nvjpg_drv_pic_param_s layout. The branch stays as pushed.
