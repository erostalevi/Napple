---
name: sweep-media-r2
batch: 2026-11-01-r2
branch: claude/sweep-media-r2-2026-11-01-r2
base: orchestrate/2026-11-01-r2
env: mac
model: claude-sonnet-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_936fd989-d4c6-4f34-ab50-b47f38dba7ca (reused media-live r1 session, opus)
launched_at: 2026-11-01
---

# sweep-media-r2: movie AAC/VP8 decoders, clean guest exit, npad device types, LAS golden route, doc rows

## Goal
Close the media AAC/VP8 gap (media −0.02 adjustment), give the guest a clean exit path, and finish round-1 follow-ups.

## Owned paths
`runtime/src/movie.c`, `runtime/src/services/{movie_audio_core.[ch],codec_headers_core.c,decoder_vt.c,decoder_null_core.c,decoder_backend.h,media_gfx_glue.c}`, `runtime/src/main.c`, `input/`, `tools/golden/`,
`runtime/tests/test_svc_movie*.c`, `test_media*`, `test_decoder_vt.c`, existing rows of `docs/architecture/runtime-services.md` for audren perf, hid GC styles and bcat (only those rows), `docs/archive/changes/2026-11-01-claude-sweep-media-r2-2026-11-01-r2.md`.

## Do not touch
ipc-depth-sys / ipc-depth-hid service files, `nvdrv*.c`, `jpeg_core.c`, `ipc.c`, gfx, sched-applets paths (kernel/thread/dispatch..., applet*.c, frontend/src, tools/soak), `orchestrations/`.

## Starting points
`runtime/src/movie.c:47-75` (decoder type enum; the Vorbis path is the model), `movie.c:721` (non-NV12 outputs), `runtime/src/main.c:143` (guest exit), `tools/golden/routes.json:16`, archived r1 integrator files for media-live and sweep-small.

## Acceptance
1. Movie-library AAC (type 4) via AudioToolbox and VP8 (type 2) via libvpx, tested with encoder-made streams (e.g. `afconvert` for AAC; build the VP8 stream with libvpx's encoder from the existing dependency, never download); non-NV12 output formats at `movie.c:721` work or each has a documented reason.
2. Guest exit (`main.c:143`) runs module finalizers, flushes saves and exits cleanly; harness test.
3. LAS `boot-to-village` golden route presses L+R at the title screen; a headless run reaches the village (verified).
4. Decision taken for you: standard npad styles report the console's device-type bits (libnx layouts), `NAPPLE_NPAD_DEVICE_TYPES=0` reverts. Decision kept: mic default stays auto with silence fallback; document it.
5. Refresh the runtime-services.md rows for audren perf, hid GC styles and bcat.
6. Four-title regression boots clean.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
