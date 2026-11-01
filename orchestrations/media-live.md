---
name: media-live
batch: 2026-11-01-r1
branch: claude/media-live-2026-11-01-r1
base: orchestrate/2026-11-01-r1
env: mac
model: claude-opus-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_936fd989-d4c6-4f34-ab50-b47f38dba7ca
launched_at: 2026-11-01
---

# media-live: verify and fix video/media decode on real titles

## Goal
Move the media and host1x items from "tested only" to verified on the Mac (+0.37 pts, and removes the media/nvdrv judgement penalties):
- `media/H.264` and `nvdrv//dev/nvhost-nvdec, vic, nvjpg, msenc`: an SDK movie or NVDEC/VIC preview path decoding real frames (VideoToolbox) in a headless run of a Testing Material title (MK8D preview panels, SMO/LAS movies; find which title opens nvhost-nvdec with `NAPPLE_IPC_TRACE`-style logging). Fix what breaks.
- `media/Vorbis/Opus movie audio`: movie audio on by default (out-of-band Vorbis/Opus header reading), AAC hookup if a title needs it.
- `media/JPEG (nvjpg / caps)`: drive JPEG through the album/caps path (screenshot save + load) in a run or an end-to-end test.
- `nvdrv//dev/nvdisp-disp0`: confirm vblank ioctls against a title that opens it (or document that none of the four does).

## Owned paths
`runtime/src/services/{nvdrv.c,nvdrv_media_core.[ch],nvdrv_media_regs.h,nvdrv_nodes_core.[ch],decoder_backend.h,decoder_null_core.c,decoder_vt.c,jpeg_core.[ch],movie_audio_core.[ch],caps.c,caps_core.c,caps_dc.c,codec_headers_core.c,media_gfx_glue.c}`,
`runtime/src/{movie.c,vt_h264.c}`, their tests in `runtime/tests/`, `docs/archive/changes/2026-11-01-claude-media-live-2026-11-01-r1.md`.

## Do not touch
`gfx/`, `runtime/src/{kernel,thread,threads,dispatch,ksession,kmsg,kcode}.c`, `runtime/src/services/applet*.c`, `audio/`, `orchestrations/`. Need a gfx change (e.g. VIC output surface upload)? Write it under "Needs from other areas".

## Starting points
`docs/archive/changes/*media-nvdec-preview*`, `*nvdrv-media-devices*`, `*nvdrv-device-nodes*`, `docs/architecture/runtime-services.md` (nvdrv/media rows), `runtime/tests/test_svc_nvdrv_media.c`, `test_decoder_vt.c`.

## Acceptance
1. `make -C runtime test` (and the media tests) pass on the Mac.
2. Verified: at least one real title decodes H.264 (or VP9) frames through NVDEC/VIC or the movie path in a headless run; frame count and a captured frame PNG in the hand-off.
3. Movie audio default-on without regressions in LAS (VP9 movie) and SMO boots.
4. JPEG album save/load round trip verified (run or e2e).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
