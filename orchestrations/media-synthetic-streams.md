---
name: media-synthetic-streams
batch: 2026-11-03-r11
branch: claude/media-synthetic-streams-2026-11-03-r11
base: orchestrate/2026-11-03-r11
env: mac
model: claude-sonnet-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: pending
launched_at: 2026-11-03
---

# media-synthetic-streams: movie AAC (type 4), VP8 (type 2) and ARGB output tested on generated streams

## Owned paths
`runtime/src/services/movie_audio_core.c`, `decoder_vt.c`, `decoder_backend.h`, `media_gfx_glue.c`, `runtime/src/movie.c`, `runtime/tests/fixtures/media/` (new), `runtime/tests/test_decoder_vt.c`, `test_svc_movie*.c`

## Do not touch
Everything else (other sessions edit runtime/src/services, runtime/src/net, gfx shader/replay/stages, media in parallel), `orchestrations/`.

## Work and acceptance
1. Generate fixtures at test time (no downloads): AAC via `afconvert`, VP8 via libvpx's encoder (already a dependency), H.264 via VideoToolbox/ffmpeg only if present; commit tiny fixtures if generation is not possible in CI.
2. Tests that run each through movie.c's decoder entries and verify frame count, dimensions, and checksum of a few frames/samples; include the ARGB (format 2) path and the non-NV12 output refusals.
3. Fix any bug found. `make -C runtime test test-services` green; four-title boots unchanged.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data read-only, lockf run lock, local-only, never ask, never push). Model: Sonnet 5.5, medium effort: work test-first in small steps, commit each.

## Result
