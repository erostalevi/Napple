---
branch: claude/media-synthetic-streams-2026-11-03-r11
orchestration: media-synthetic-streams
base: 281f4d3483540816dea22517d4d3d9a263adfb67
head: HEADPLACEHOLDER
env: mac
date: 2026-11-03
areas: [runtime/media]
status: ready
---

# media-synthetic-streams: AAC / VP8 / ARGB on generated streams

## Summary
On inspection the assignment was already met by the base commit (f0de6ab): `runtime/tests/test_svc_movie_host.c` generates its streams at test time
(AAC with AudioToolbox, VP8/VP9 with the libvpx encoder, H.264 with VideoToolbox in `test_decoder_vt.c`), drives movie.c's decoder entries and checks frame
counts, dimensions, per-channel tones, PSNR against the source, ARGB (format 2) against the library's conversion formula, and the refusals (video in PCM
format 1, type left to the library, VP8 decoder given a VP9 mime). `test_svc_movie_audio.c` covers the audio core. I ran all three and they pass; no bug
found, no source change made. No fixtures needed committing (everything is generated). Nothing was added: I did not invent redundant tests.

## Files
None changed (only this integrator file).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Generated AAC (stereo csd-0, ADTS, no csd-0, 5.1 order, corrupt unit) | verified | `build/tests/test_svc_movie_host`: 1015 checks, 0 failed |
| Generated VP8 NV12 / ARGB, VP9 ARGB, frame count + size + PSNR | verified | same run: PSNR 51.6 / 47.9 / 44.0 dB over 30 frames |
| Non-NV12/ARGB output refusals | verified | same run (format 1 refused, type 4 format 4 left to the library, VP9 mime on VP8 refused) |
| Audio core | verified | `test_svc_movie_audio`: 54 checks, 0 failed |
| H.264 VideoToolbox decode, pitched, encoder | verified | `test_decoder_vt`: 130 checks, 0 failed |
| `make test-services` as a whole | untested | needs `rustc` for napple-room (not on PATH in this shell); movie tests are picked up by the `tests/test_svc_*.c` wildcard in tests/services.mk |
| Four-title boots | untested | no runtime code changed |

## Unbuilt / untested list
None.

## Findings
- Tests need `make -C ../gfx adapter` plus frontend/input/audio libs built first (otherwise a link error on gfx_napple.o / napple_frontend).
- `test_decoder_vt.c` has -Wmissing-field-initializers warnings (DecodeRequest pitch fields); harmless.

## How to integrate
No ordering constraints; no conflicts (docs-only change).

## How to test
1. `make -C gfx adapter; make -C frontend; make -C input; make -C audio; cd runtime && make build/tests/test_svc_movie_host build/tests/test_svc_movie_audio build/tests/test_decoder_vt` then run each; pass = `"failed":0` / `0 failed`.

## Fix policy hints
Nothing to fix.

## Needs from other areas
None.
