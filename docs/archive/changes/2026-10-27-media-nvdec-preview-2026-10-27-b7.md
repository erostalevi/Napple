# media-nvdec-preview (2026-10-27, batch b7): MK8D's preview videos play through nvhost-nvdec + VIC

Branch `claude/media-nvdec-preview-2026-10-27-b7` (Mac session, base `6f70b92`). **Verified on this Mac with MK8D (base + update + DLC, headless, audio off, under the
game lock): the Grand Prix mode panel and the cc-select panel, green before, now play their preview video cleanly, with no macroblock corruption.**

## The preview: three faults, none of them the b6 candidates
b6 left three candidates (nvgpu AS mapping of nvmap handles with gfx aliasing, the VIC config, block-linear output). A `NAPPLE_MEDIA_TRACE` run ruled out
the first and the third, and found the real faults in this area:
1. **VIC config address.** Method 0x1c1 is SetControlParams. MK8D's `0x610110` there is the control word, which is why it "resolved nowhere". The config struct
   offset is method 0x1c2 (the public emulator sources use this numbering), and it resolves to a pinned handle. Its output surface word (+0x20) says
   **NV12, pitch linear, 512x608**. Block-linear swizzle: ruled out for this title. The game samples the VIC output where the VIC writes it: no gfx or nvgpu AS
   link is missing, so candidate (a) is ruled out too.
2. **NVDEC output slot.** Register 0x103 is a running frame number (0, 1, 2, ...), not the DPB slot. After 16 frames the slot-register lookup was 0, and every
   later decode failed with "unmapped surface". The VIC then kept converting stale or never-written slots: a frozen first frame, or green (Y=U=V=0). The slot
   is `curr_pic_idx` in the H.264 picture-info structure (word 45 bits 2-8: the emulator's 64-bit layout shifted by 8, as b6 found for the low fields). The trace
   confirms that it follows the slot registers the SDK rewrites, with non-reference B pictures reusing one slot.
3. **Reference frames in the synthesized SPS.** It claimed 16, and level 3.0. With 16 references the DPB, capped at 16 frames, has no room for the current
   non-reference picture, so VideoToolbox evicts a reference the stream still uses. The result was macroblock corruption mid-GOP, deterministic and cleared at the
   next IDR. Found by replaying the dumped stream (`NAPPLE_NVDEC_DUMP`) offline through `vt_h264`: 6..15 references give byte-identical frames over all
   286 pictures, ≤5 fail (the stream needs 6), and only 16 corrupts. Now 15, with the smallest level whose DPB holds 16 frames (3.2 at 1216 MBs).

Evidence (run logs in the session scratchpad; the frames are in the shared notes folder, never in the repository): engine stats `class 0xf0: 240 executes, 239 frames,
0 errors`; before fix 2, `240 executes, 17 frames, 222 errors (last 2)`.

## Other items of the assignment
- **Decoder backend ABI: pitch != width.** `DecodeRequest.luma_pitch/chroma_pitch` (0 = packed) come from the picture-info pitches (words 35/36). The new
  `decoder_write_nv12()` writes rows at the pitch and leaves the padding alone, and the null and VideoToolbox backends use it. `test_decoder_vt`: a pitched decode
  gives the same checksum as the packed one, the padding stays untouched, and a surface one row short is refused (107 -> 124 checks).
- **VIC rewritten on the real layout**: RGBA8 (0x1f), BGRA8 (0x20), RGBX8 (0x23), NV12 (0x44); pitch-linear (RGB rows width*4, NV12 rows aligned to 256) or
  block-linear output (GOB layout, block height from the config). BT.601 limited range. The old reduced 9-word Napple config structure is gone. The NV12 copy stays
  for streams without a readable config.
- **VP9 on the VT backend**: refused cleanly at `open()`. The engine reports "codec not supported" in the status structure and in a new summary line (media engine
  failures per channel). libvpx stays in movie.c. The policy is written in `decoder_vt.c`.
- **Movie audio**: Vorbis was already on by default, with out-of-band headers (csd-0..2). The movie library has no Opus decoder type (DecoderTypeToMime /
  IsDecoderTypeSupported cover 1..5), so Opus stays core-only. `test_svc_movie_audio` now checks decoded sample counts against the streams' last granule (Vorbis
  mono and stereo, Opus with pre-skip; 40 -> 43 checks). This needs no audio device.
- **JPEG through caps**: new `caps:dc` service (`caps_dc.c`, constructor-registered so services.c is untouched). 3001 DecodeJpeg decodes through jpeg_core into
  the requested size (box-scaled when the file differs) via `napjpeg_decode_into`. Tests: `test_svc_caps_dc` (13 checks), `test_svc_jpeg` 43.
- **nvdrv.c**: the stale "movie playback ... does not go through here" comment is corrected. Debug: `NAPPLE_MEDIA_TRACE` (nvmap allocations/pins/relocations,
  unresolved or non-guest addresses, engine registers, NAL types, periodic stats) and `NAPPLE_NVDEC_DUMP`.
- **Channel / nvdisp ioctl completeness**: no new ioctl numbers. The channel table covers the numbers the public emulator handles (as far as the author remembers them;
  unverified), and no unimplemented ioctl appeared in the MK8D runs (an unknown one is still logged and listed in the summary). The nvdisp ioctls stay as proposed.

## Verified / tested / not done
- Verified (MK8D, this Mac): both preview panels play; 0 NVDEC errors over 240 decodes; golden route result in the integrator file.
- Tested: `make -C runtime test test-services test-host` green.
- Not verified: other titles' nvdec streams (only MK8D sends H.264 to nvhost-nvdec among the test titles); RGB/block-linear VIC output (MK8D uses NV12 pitch
  linear; unit tests only); caps:dc on a title (none of the four test titles opens it).

## Files
`runtime/src/services/{nvdrv.c, nvdrv_media_core.[ch], nvdrv_media_regs.h, decoder_backend.h, decoder_null_core.c, decoder_vt.c, jpeg_core.[ch], caps_dc.c (new)}`,
`runtime/tests/{test_decoder_vt.c, test_svc_nvdrv_media.c, test_svc_jpeg.c, test_svc_movie_audio.c, test_svc_caps_dc.c (new)}`,
`docs/architecture/runtime-services.md`, `docs/reference/environment-variables.md`.
