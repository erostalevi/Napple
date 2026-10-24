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
