# nvjpg-headerless-redo (2026-11-03, round 11)

Header-less NVJPG decode, rebuilt on the real `nvjpg_drv_pic_param_s` layout of round 10. The earlier attempt (`claude/nvjpg-headerless-2026-11-03-r11`) was written
against the round-9 reduced register model and stays unmerged; only its library half was taken.

- **jpeg_core** (cherry-picked from that branch): `decode_frame` split out of the headered decoder, and `napjpeg_decode_raw_rgba` (`JpegRawParams`: geometry,
  sampling, quantiser and Huffman tables, restart interval, plus the entropy-coded segment). `test_svc_jpeg_raw`: 64 checks, header-less decode pixel-exact with the
  headered decode for the libjpeg vectors and encoder output (4:2:0, 4:4:4, 4:2:2, grey, restart intervals, odd sizes).
- **nvdrv_media_core**: a decode whose bitstream buffer has no SOI in front of `bitstream_offset` and whose `dec_frame_size` is programmed is decoded from the
  structure's decoder half (`nvjpg_raw_params`, `nvjpg_huff`): frame size, stream chroma mode, restart interval, `dec_block_ctl`, `dec_quant`, Huffman
  mincode / symbol. The output stage (RGBA/BGRA/ABGR/ARGB, YUV planar / semi-planar, YUY2, downscale, subsample, alpha, strides) is shared with the headered path,
  so both produce the same surface. No SOI and no frame size is still BAD_MAGIC. No register was added: the old reduced model (0x101..0x109, table block 0x105) is not reintroduced.
- **Table semantics** are Napple's reading of fields whose positions open-gpu-doc pins but whose meaning it does not state (listed in
  `docs/reference/nvdrv-layouts.md`): nothing was traced from a title, so a real driver could differ (for instance in the slot numbering or in what a Huffman
  `idx` counts). Layout facts only, no GPL code.
- **Tests** (`test_svc_nvdrv_media`, through the ioctl / host1x stream path): stripped baseline vectors (4:4:4, 4:2:0, 4:2:2, flat, grey, restart interval) and generated
  odd-size images decode pixel-exact against the headered decode; planar YUV equal to the headered planes; a padded BGRA pitch with `bitstream_size` 0; seven refusals
  (no frame size, grid mismatch, sampling mismatch, slot above 2, non-canonical mincode, no tables, chroma mode 6).
- Not covered: 4:4:0 (422V) has no vector in the corpus (the sampling table maps it; the core handles 1x2 sampling); 16-bit quantiser tables cannot be expressed in the hardware's 8-bit `dec_quant`.
