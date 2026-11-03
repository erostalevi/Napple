# nvdrv media devices (b8): pitches, VP9, NVJPG encode, MSENC

Branch `claude/nvdrv-media-devices-2026-10-28-b8`. Linux cloud session.

## Changed
- `nvdrv_media_core.c`: NVDEC records output pitches per surface; VIC input uses them (fallback unchanged). VP9 picture-info pitches (proposed words 20/21, guarded). NVJPG encode mode. MSENC encode through `EncoderBackend`.
- `decoder_backend.h` / `decoder_null_core.c`: `EncoderBackend` (install/current/null encoder), `vp9_superframe_split`.
- `decoder_vt.c`: VP9 decode session (opt-in `NAPPLE_VT_VP9=1`), H.264 `VTCompressionSession` encoder (`encoder_backend_videotoolbox`, installed by a constructor).
- `nvdrv_media_regs.h`: NVJPG encode words, MSENC registers, VP9 pitch words.
- Tests: `test_svc_nvdrv_media.c` (+41 checks), `test_decoder_vt.c` (encoder, Mac only).

## Verified vs not
- tested (Linux): everything portable; `sh tools/linux-check/run-tests.sh runtime/tests/test_svc_nvdrv_media.c runtime/tests/test_svc_jpeg.c` -> 169 + 43 checks, 0 failed. Mutation check: disabling the VIC pitch hint fails the pitch tests.
- unbuilt: the VideoToolbox parts of `decoder_vt.c` and the new `run_encoder` test.
