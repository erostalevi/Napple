# h264-decoder-type (2026-10-24, batch plan8)

Read from the movie library itself (LAS subsdk1, `movie::DecoderImpl::DecoderTypeToMime` at +0x1d470, `IsDecoderTypeSupported`, `IsOutputFormatSupported` at +0x1d6d0; disassembled with objdump):

| DecoderType | mime |
|---|---|
| 1 | video/avc (H.264) |
| 2 | video/x-vnd.on2.vp8 |
| 3 | video/x-vnd.on2.vp9 |
| 4 | audio/mp4a-latm (AAC) |
| 5 | audio/vorbis |

Types 1..5 are supported. Output format 1 exists only for the audio types 4/5 (PCM); video types take format 4 or 2 (buffer mode) / 3 (texture mode).
So the type-5 / format-1 / buffer-mode decoder LAS creates is a **Vorbis audio decoder**, not AVC; no output format needs adding for H.264, and it stays with the library
(LAS log: `library decoder ... configure: mime audio/vorbis 0x0 sample-rate 48000`).

Changes (`runtime/src/movie.c`): default AVC type 0 -> 1 (`TYPE_AVC`, `NAPPLE_MOVIE_H264_TYPE` still overrides); comment documents the table; Configure now logs the mime of
decoders left to the library and of host decoders before the format check (identifies unknown types).

Verified (Mac, headless, LAS 45 s, default env): exit 0; VP9 type 3 decoder on the host (1500+ frames), type 5 left to the library, Vorbis configure logged. `make -C runtime test`: test_media 56 checks, 0 failed.
NOT verified: any title that actually creates an AVC (type 1) decoder (none known; SMO creates no decoder in scripted paths); the type-1 path against a guest. Type 4 (AAC) is also left to the library.
Note: with `NAPPLE_MOVIE_H264_TYPE=5` the host would wrongly take the Vorbis decoder; do not use it.
