# Third-party notices

Napple reuses and adapts code from MIT-licensed projects. The notices below must be kept with any distribution.
(Created by the audio/input/frontend work because the file did not exist yet; the runtime engineer may extend it.)

## Ryujinx.Audio (MIT)

Used for: audio renderer wire formats, voice/wave-buffer state machine, reverb3d, delay, standard reverb, compressor,
limiter, aux and capture effect algorithms (`audio/src/reverb.c`, `audio/src/effects.c`, parts of `audio/src/mix.c`,
`audio/src/audren.c`).
Copyright (c) Ryujinx Team and Contributors. Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files, to deal in the Software without restriction, including without
limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
subject to the above copyright notice and this permission notice being included in all copies or substantial portions.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

## MacNativeGames / TOTK runtime (MIT)

Used for: adapted audren mixer and renderer structure, hwopus wrapper, GameController/CoreHaptics rumble approach,
face-button layout rules, swkbd UTF conversions (`audio/`, `input/src/gamepad.m`, `frontend/src/settings.c`).
Copyright (c) MacNativeGames contributors, MIT License.

## LASRecomp (MIT)

Used for: reference of keyboard/controller binding design.
Copyright (c) LASRecomp contributors, MIT License.

## libopus (BSD 3-clause) and libvpx (BSD 3-clause)

Linked dynamically/statically from Homebrew (`opus`, `libvpx`); not copied into this repository.

## Runtime core (`runtime/`, `tools/refpy/`) (MIT)

- MacNativeGames contributors (MIT, Copyright (c) 2026 MacNativeGames contributors): thread/gate/host-stack design, module
  relocation and memory-permission handling, guest memory layout and crash handling, reimplemented and generalized in `runtime/src/`.
- d2r-mobile (MIT, Copyright (c) 2026 jhaizhou): NSO rewriter stopgap in `tools/refpy/` (license text in `tools/refpy/LICENSE`, with
  local changes), and `runtime/src/gate.S`, `runtime/src/thread_enter.S`, `runtime/src/hle_thunks.S`.
- LASRecomp (MIT, Copyright (c) 2026 Peiikai): knowledge of the Horizon services Link's Awakening uses (no code copied).
- The HIPC/CMIF framing in `runtime/src/ipc.c` follows the public libnx description (ISC); no libnx code is included.

MIT License text: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions: The above copyright notice and this permission notice shall
be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

## runtime/src/movie.c, movie.h
Adapted from MacNativeGames (MIT), games/totk/runtime/movie_host.c/.h: host VP9 decoder behind the SDK's movie::Decoder (libvpx, BSD-3-Clause, linked statically).

## nxnative (MIT, copyright seedds): test and documentation ideas
`gfx/tests/t_clear_ds.c`, `t_clear_tex.c`, `t_depth16.c`, `t_feedback.c`, `t_copy_alias.c`, `t_present_snap.c`, `t_pso_archive.c`, `tools/check_docs.py` and `.github/workflows/tests.yml`
follow the structure and test ideas of nxnative (`dm/gfx/tests/t_*.m`, `dm/tools/tests/test_docs.py`, `.github/workflows/tests.yml`; https://github.com/seedds/nxnative, MIT licence). They are
written fresh against this repository's NVN entry points; no source lines were copied. Copyright (c) seedds, MIT licence text: see nxnative/LICENSE.

## crates/napple-rewrite (jumptab.rs shapes, windows.rs adrp, survey.rs): nxnative cases
Behaviour cases (jump-table shapes incl. halfword tables padded to the largest entry, adr-relative offset tables, adrp inside exclusive windows) and the pre-flight survey idea follow the documented cases in nxnative (MIT, Copyright (c) seedds), docs/reference/rewriter.md, dm/games/profile_guess.py and scripts/build_ipa_from_nsp.py. The Rust code is written from those descriptions; no nxnative source text is copied.

## nx-kernel (2026-10-26)
Behaviour (not code) of nxnative (MIT, copyright seedds) docs/reference/threads.md and services.md was used as the specification for the same-core handoff, sleep_precise, ideal-core -3, history-ring prefill, shake and offline push-service stand-ins in runtime/src/threads.c, input/src and runtime/src/services/push.c. No nxnative source was copied.

## gfx/src/shader (envytools, 2026-10-26)
The bit layouts of the video ALU ops (VADD, VMAD, VABSDIFF, VSHL, VSHR, VSET, VSETP), FCHK, PIXLD, IMAD/IMAD32I, DSET and the call/jump family in maxwell_translate.c were checked against the Maxwell
disassembler tables of envytools (envydis gm107.c, MIT, Copyright 2014 Red Hat Inc., https://github.com/envytools/envytools). Only the encoding facts (bit positions and operand roles) were read; no source text was copied.

## stb_vorbis (public domain / MIT, Sean Barrett)

`runtime/src/third_party/stb_vorbis.c` (v1.22) is vendored for the host Vorbis movie-audio decoder (`runtime/src/services/movie_audio_core.c`). The
file carries its own dual public-domain / MIT notice. One local change: the CRC and granule reads in the push-data page scan cast to `uint32` before
shifting by 24 (signed-shift undefined behaviour under UBSAN, no change in behaviour).
