# Change: audio, input and frontend libraries

Date: 2026-10-21. Owner: audio/input/frontend engineer. Interface for integrators: `docs/audio-input-frontend-interface.md`.

## Objective

Provide the host side of audio, input and the window for Napple as three static libraries with plain C headers,
independent of IPC marshaling and of any particular game (`audio/`, `input/`, `frontend/`). Nothing under `runtime/`
or `gfx/` was modified.

## Files

| Path | Purpose |
|---|---|
| `audio/include/napple_audio.h`, `audio/src/*.c` | audren core (`audren.c` update parser and voice state machine, `mix.c` mixer, `reverb.c`/`effects.c` DSP), output engine (`engine.c`), audout (`audout.c`), hwopus (`hwopus.c`), `util.c` |
| `audio/tests/` | `test_audren`, `test_effects`, `test_audout`, `test_hwopus`, `test_engine`, `test_threads`, `demo_sine`, `wire.h` (blob builder) |
| `input/include/napple_input.h`, `input/src/` | `hid_shm.c` (pure shared memory writers), `input.c` (mapping, thread, files), `bindings.c` (defaults, validation, JSON), `names.c`, `motion.c`, `gamepad.m` (GameController, CoreHaptics), `tinyjson.h` |
| `input/tests/` | `test_hid`, `test_bindings`, `test_motion`, `test_input`, `demo_pads.m` |
| `frontend/include/napple_frontend.h`, `frontend/src/` | `frontend.m` (window, view, menu, focus, screenshot, vsync), `overlay.m`, `textbar.m`, `settings.c` (pure: settings JSON, paths, text validation, UTF, frame limiter), `tinyjson.h` |
| `frontend/tests/` | `test_settings`, `test_window.m`, `demo_window.m` |
| `docs/audio-input-frontend-interface.md` | interface and integration instructions |

## Logic

* **audio**: `napple_audren_request_update` validates every section of the update blob against the header (sections
  may be unaligned: copied with memcpy), applies voice/mix/effect/splitter/sink parameters, and writes the output blob.
  A render thread (or the caller in manual mode) mixes one frame (240 samples) per tick under the renderer lock:
  voices decoded along the exact sample path the voice clock consumes, linear resampling, Q14/float biquads, volume,
  channel resource or splitter routing, submixes deepest first, effects in order, device or circular sink. Output goes to
  a per-source lock-free ring; one Core Audio unit sums sources at 48 kHz stereo with master volume and clipping.
  Latency: prefill 20 ms (default), device buffer requested 256 frames, backlog trimmed after stalls. audout is a
  5 ms pacing thread feeding the same engine; hwopus wraps libopus (single and multistream).
* Guest-caused protocol violations call `napple_audio_fatal`, which longjmps to the public entry (request update) or
  to the render loop; the host never aborts because of guest data (the renderer is muted after a render-path error).
* **input**: `napple_input_update` merges keyboard, mouse and GameController state per npad slot, picks the style from
  the guest-supported set, and pushes states into the rings with `hid_shm.c`. Ring semantics: header
  {timestamp, 17, tail, count}, entries {outer sampling number, state}. Bindings: `input.json` per game, atomic write,
  invalid files are never overwritten. Motion: complementary filter (quaternion) -> direction matrix, angle,
  angular velocity in revolutions/s. Rumble: CoreHaptics engines per handle fed by a 10 ms timer.
* **frontend**: NSWindow with a layer-backed view whose layer is a `CAMetalLayer`; events forwarded to input; overlay and
  text bar are subviews (work in full screen); settings applied on the main thread, saved atomically, announced by callback.

## Decisions

* Reuse of Ryujinx.Audio-derived DSP (reverb3d, delay, reverb, compressor, limiter) and wire formats through the MIT
  reference ports (see THIRD_PARTY_NOTICES.md), restructured; TOTK-specific diagnostics and env switches removed.
* Output engine is shared (one Core Audio unit), not one unit per renderer, so audren + audout + several renderers do
  not fight over the device.
* `tinyjson.h` is duplicated in input/ and frontend/ (independent libs); both `make test` check the copies are identical.
* Layout offsets are data (`napple_hid_layout`) because only part of the Npad entry could be verified offline.
* The overlay is a subview rather than a child window: simpler focus handling, works in full screen.

## Risks and assumptions (honest list)

1. **HID Npad entry offsets past the first lifo are reconstructed from public documentation and not validated against a
   running title** (`device_type`, system properties, battery are written at derived offsets; `device_type` and
   `system_properties` are written as zero). Verification plan: run LAS/SMO with `NAPPLE_HID_DUMP` and compare the
   offsets the game reads.
2. Frame-rate cap, internal resolution and vsync are only settings plus helpers: gfx and the runtime must apply them.
3. DualSense/Pro controller paths (GameController mapping, motion, haptics) were written but could not be run: no
   controller was attached to the test Mac. Keyboard/mouse paths are tested through synthetic events.
4. audren: performance metrics are not produced; voice drop never triggers; REV1..4 headers are accepted but untested.
5. HwOpus: the library does not call `write_prepare` on the PCM buffer; the runtime must (see interface doc).
6. Screenshot fallback uses `screencapture -l`, which may need Screen Recording permission; gfx can provide a frame via
   `on_screenshot`.
7. Movie (VP9) decoding from the reference (`movie_host.c`) is out of scope of this change; libvpx is installed
   (`brew install libvpx`) for a follow-up.
8. THIRD_PARTY_NOTICES.md at the repo root must credit Ryujinx (MIT, audio DSP and layouts), MacNativeGames/TOTK runtime
   and LASRecomp (MIT) for the reused parts.

## Tests executed

| Command | Result |
|---|---|
| `make -C audio test` | audren 3410 checks, effects 1253, audout 506, hwopus 22, engine 11, threads 2: all pass |
| `make -C audio asan` / `tsan` | clean (UBSan, ASan, TSan) after fixing unaligned section reads and unlocked getters |
| `NAPPLE_TEST_DEVICE=1 build/test_engine`, `make -C audio demo` | real Core Audio output opened (48 kHz, 256-frame buffer), 0 underruns / 0 overruns in the 1.5 s demo, measured latency about 24 ms |
| `make -C input test` | hid 182, bindings 22 (incl. 20k fuzz iterations), motion 16, input 22: all pass |
| `make -C frontend test` | settings 30 (fuzz, traversal, UTF, limiter), window 13 (real AppKit loop: Metal layer, settings persistence and reload, overlay callbacks, scripted swkbd, quit path): all pass |
| `NAPPLE_DEMO_OVERLAY=1 build/demo_window` + `screencapture -x` | window with colour-cycling CAMetalLayer at 180 fps and the overlay (Video tab) rendered correctly; Controls/Game tabs and the swkbd bar were not exercised interactively |

Recommended, not done: run with the real games; fuzz `napple_audren_request_update` with captured update blobs; test
Bluetooth/Pro Controller rumble; long-run (hours) audio underrun statistics.
