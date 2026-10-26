# media-services (cloud2, 2026-10-26): media engines, JPEG, Vorbis/Opus

Branch `claude/media-services-2026-10-26-cloud2` (Linux cloud session; base `e856e49`). Everything here is **tested on Linux, unbuilt on macOS and unverified with a real title**.
Architecture and tables: [runtime-services.md, "Media services"](../../architecture/runtime-services.md). Hand-off: `integrators/claude-media-services-2026-10-26-cloud2.md`.

## What changed
1. **nvdrv multimedia devices** (`nvdrv.c`, new `nvdrv_media_core.[ch]`, `nvdrv_media_regs.h`, `decoder_backend.h`, `decoder_null_core.c`, `codec_headers_core.[ch]`).
   SUBMIT command buffers are now parsed as host1x streams (all eight opcodes, relocations, nested gathers) and run on register-level engines for NVDEC (to a `DecoderBackend`),
   VIC (NV12 to RGBA), NVJPG (real JPEG decode) and MSENC (accepted). Handles without a mapped address behave as before, so the existing ioctl behaviour is unchanged
   (the 29 checks of `tests/host/test_nvdrv.c` pass unmodified).
2. **Portable JPEG codec** (`jpeg_core.[ch]`): baseline decoder (grey, 4:4:4/4:2:2/4:4:0/4:2:0, restart intervals) and encoder, box-filter downscale.
3. **Caps images** (`caps_core`, `caps.c`): a save with a 1280x720 RGBA input buffer is stored as JPEG; size, `LoadAlbumFile` (JPEG), image (110) and thumbnail (120, session 5) loads work; entries without an image keep the old 0-byte behaviour.
4. **Movie audio** (`movie_audio_core.[ch]`, vendored `third_party/stb_vorbis.c`, `movie.c`): Vorbis from raw MediaCodec-style packets (wrapped into Ogg pages for stb_vorbis push-data; headers in-band or csd-0 Xiph) and Opus (libopus) to s16 PCM.
   `movie.c` hosts type 5 / PCM / buffer-mode decoders on it **only with `NAPPLE_MOVIE_VORBIS=1`** (default off: it was never run against the SDK library, and LAS currently uses the library's own Vorbis).
5. Stretch: VP9 uncompressed-header parsing, VP8 frame tag, H.265 SPS skeleton (all in `codec_headers_core`, tested on ffmpeg-made streams).
6. Tooling: `tools/linux-check/check-movie.sh` (+ stubs in `tools/linux-check/movie/`) syntax-checks `movie.c` on Linux. `runtime/Makefile`: `HOST_MEDIA` sources for `test_nvdrv`, new host test `test_nvdrv_media`, `-I`/`-DNAPPLE_HAVE_OPUS` flags for the Homebrew libopus.

## Tests (Linux, gcc, also clean under ASAN+UBSAN; jpeg and movie_audio also under valgrind)
| Test | Checks | Covers |
|---|---|---|
| `test_svc_nvdrv_media.c` | 52 | NVDEC H.264/VP9/H.265 through the ioctl handlers, picture slots, status structure, unsupported codec, unmapped surface, relocations, gathers, malformed streams, unknown registers, recording backend, VIC (red/white/ARGB/scaling/pitch/error), NVJPG (RGBA/NV12/scale/pitch/garbage/small buffer), MSENC |
| `tests/host/test_nvdrv.c` (old, run through a scratch shim) | 29 | open/nvmap/channel/syncpoint/submit/map ioctls unchanged |
| `test_svc_jpeg.c` | 38 | encode/decode round trips (4 kinds), 1280x720, Pillow/libjpeg vectors (4:4:4, 4:2:0, 4:2:2, grey, restart intervals, flat), progressive refused, truncated, corrupt |
| `test_svc_caps_image.c` | 100 | JPEG store, size, file/image/thumbnail loads through caps:u and the session, errors, delete, album cap with images |
| `test_svc_caps.c` (existing) | 150 | unchanged behaviour of the caps tables |
| `test_svc_movie_audio.c` | 21 (24 with libopus) | Vorbis mono 22.05 kHz and stereo 44.1 kHz sines (tone and level measured), csd-0 Xiph blob, errors, flush/resume; Opus with `-DNAPPLE_HAVE_OPUS -lopus` |
| `test_svc_codec_headers.c` | 15 | H.264 SPS (baseline, high, cropped, 4-byte start code, truncated), NAL histogram, VP9 key/inter/show-existing, H.265 SPS |

## Decisions
- Register offsets, codec ids and the VIC/NVJPG structures are **from memory** (no documentation was available offline); all offsets are in `nvdrv_media_regs.h`, the VIC and NVJPG structures are deliberately reduced Napple layouts. Unknown register writes are counted and summarised so a Mac run with `-v` fixes the map.
- A malformed host1x stream or an unmapped gather fails the SUBMIT (like the kernel firewall); a failed decode does not (status structure + counter).
- Engines run inside the nvdrv lock (simple; a slow backend would block other nvdrv calls: revisit when the VideoToolbox backend lands).
- jpeg_core is portable C instead of ImageIO so caps and nvjpg are testable on Linux and identical on the Mac; `vt_h264.c`'s ImageIO helpers stay for movie.c.
- stb_vorbis (public domain) vendored with one UBSAN-motivated cast fix (THIRD_PARTY_NOTICES.md). No new GPL.
- Vorbis in `movie.c` is opt-in because the SDK's actual calling pattern (csd as MediaData buffers vs. in-band config packets) could not be observed.

## Not done / Mac checks
- VideoToolbox (H.264) and libvpx (VP9) `DecoderBackend` for NVDEC: not written; needs H.264 SPS/PPS synthesis from the NVDEC picture-info structure. Interface and null backend are in place.
- NVDEC output is pitch-linear (hardware: block-linear).
- Mac: compile everything (`make -C runtime`, `make -C runtime test-services test-host`), run the four titles (LAS, SMO, ACNH, MK8D) headless 45 s with `-v` and read the nvdrv summary lines; then try `NAPPLE_MOVIE_VORBIS=1` on a title that plays movie audio.
