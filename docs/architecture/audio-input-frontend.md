# audio / input / frontend interface (v1)

For the runtime engineer. Three static libraries, plain C headers, no IPC knowledge: the runtime's IPC layer parses
requests, passes raw guest bytes in and marshals results out. Everything is game-agnostic; per-game data is only the
title id and the user's settings/bindings files.

| Library | Header | Build | Frameworks |
|---|---|---|---|
| audio | `audio/include/napple_audio.h` | `make -C audio` -> `audio/libnapple_audio.a` | AudioToolbox, CoreAudio, Foundation; libopus (`brew install opus`) |
| input | `input/include/napple_input.h` | `make -C input` -> `input/libnapple_input.a` | GameController, CoreHaptics, Foundation |
| frontend | `frontend/include/napple_frontend.h` | `make -C frontend` -> `frontend/libnapple_frontend.a` | AppKit, QuartzCore, CoreVideo, Metal |

Link line for the app: `-Lfrontend -lnapple_frontend -Linput -lnapple_input -Laudio -lnapple_audio
/opt/homebrew/opt/opus/lib/libopus.a -framework Cocoa -framework QuartzCore -framework CoreVideo -framework Metal
-framework AudioToolbox -framework CoreAudio -framework GameController -framework CoreHaptics -framework Foundation`.
`make -C <dir> test` runs the headless tests; `make -C <dir> demo` builds the interactive demo.

Logging: every line is prefixed `[napple]` (`[napple] audio: ...`, `[napple] input: ...`, `[napple] frontend: ...`).

## 0. Process layout

```
main thread     napple_frontend_run()  (AppKit loop; on_ready fires once the window + CAMetalLayer exist)
 on_ready ->    start guest threads, gfx (napple_gfx_config.metal_layer = napple_frontend_metal_layer(fe))
audio threads   audren render thread (5 ms frames, per renderer), audout pump thread, Core Audio callback (real time)
input thread    napple_input thread writes HID shared memory at 120 Hz
```

## 1. audio

### 1.1 IPC services -> library calls

| Service / command | Call |
|---|---|
| `audren:u` GetWorkBufferSize(param) | `napple_audren_work_buffer_size(param, 0x34, &size)` (tiny constant; the host keeps its own state) |
| `audren:u` OpenAudioRenderer(param, workbuf size, aruid, [handles]) | `napple_audren_open(param, 0x34, &host, 0, &r)`; then create the IAudioRenderer session around `r` |
| `audren:u` GetAudioDeviceService / ...ByRevision | session around the `napple_audio_device_*` functions |
| IAudioRenderer GetSampleRate / SampleCount / MixBufferCount / State | `napple_audren_sample_rate / sample_count / mix_buffer_count / state` |
| IAudioRenderer RequestUpdate (cmd 4) / RequestUpdateAuto (10) | `napple_audren_request_update(r, in, in_size, out, out_size, &written, perf, perf_size)`; out buffer is zero-filled and `written` bytes are meaningful |
| IAudioRenderer Start (5) / Stop (6) | `napple_audren_start / stop` |
| IAudioRenderer QuerySystemEvent (7) | create a Horizon event; `host.frame_event` signals it after every rendered frame |
| IAudioRenderer SetRenderingTimeLimit (8) / GetRenderingTimeLimit (9) | `napple_audren_set_time_limit / get_time_limit` |
| IAudioDevice ListAudioDeviceName, GetActiveAudioDeviceName | `napple_audio_device_name` |
| IAudioDevice Set/GetAudioDeviceOutputVolume | `napple_audio_device_set_volume / get_volume` |
| IAudioDevice GetActiveChannelCount | `napple_audio_device_channel_count` (2) |
| `audout:u` OpenAudioOut(rate, channels) | `napple_audout_open(rate, ch, &host, &o, &granted_rate, &granted_ch)`; reply (granted rate, channels, format=PCM16 (2)) |
| IAudioOut Start/Stop/GetState | `napple_audout_start / stop / state` |
| IAudioOut AppendAudioOutBuffer(tag, AudioOutBuffer) | `napple_audout_append(o, tag, buf_ptr, data_size, data_offset)`; `host.buffer_event` signals the buffer event |
| IAudioOut GetReleasedAudioOutBuffers / ContainsAudioOutBuffer | `napple_audout_get_released / contains` |
| `audin:u` ListAudioIns(Auto, AutoFiltered) / OpenAudioIn(Auto)(rate, channels) | `napple_audin_device_name` ("BuiltInHeadset", always listed); `napple_audin_open(rate, ch, &host, &i, &granted_rate, &granted_ch)`, 0 = 48000 Hz / 2 ch, 1-2 channels, 8-96 kHz; reply (granted rate, channels, format=PCM16 (2), state=stopped (1)) |
| IAudioIn Start/Stop/GetState, Set/GetAudioInDeviceGain (float, 0..2) | `napple_audin_start / stop / state / set_gain / get_gain` |
| IAudioIn AppendAudioInBuffer(Auto)(tag, AudioInBuffer) | `napple_audin_append(i, tag, buf_ptr, data_size or capacity when data_size is 0, data_offset)`; at most 32 buffers queued or released-but-uncollected; `host.buffer_event` fires when one is full |
| IAudioIn GetReleasedAudioInBuffer(sAuto) / ContainsAudioInBuffer / GetAudioInBufferCount / FlushAudioInBuffers | `napple_audin_get_released / contains / buffer_count / flush`; unused tags zero-filled as for audout. Not implemented: OpenAudioInProtocolSpecified (5), the UAC appends (7, 10) |
| capture source | `napple_audin_set_source(read, user)`, mono float at the stream rate; none installed = silence. The runtime installs none (no Core Audio input, no `NSMicrophoneUsageDescription`) |
| `hwopus` GetWorkBufferSize / OpenHardwareOpusDecoder | `napple_hwopus_work_buffer_size`, `napple_hwopus_open(rate, ch, &d)` (multistream: `..._ms`) |
| IHardwareOpusDecoder DecodeInterleaved* | `napple_hwopus_decode(d, packet, size, pcm, pcm_bytes, &consumed, &samples)` |
| nn::codec HardwareOpusDecoder (guest SDK symbols, when a game links them) | same functions; no IPC needed |

Guest memory is reached through `napple_audio_host.translate` (NULL = identity, the normal case). Set
`write_prepare` to `napple_gfx_guest_write_prepare` wrapper: the library calls it before writing into guest memory
(circular sink buffers, aux/capture rings, hwopus PCM).

Error mapping: `NAPPLE_AUDIO_E_PROTOCOL` for a malformed update is a guest bug: log and return the Horizon
`ResultInvalidUpdateInfo` (module 153, description 41) or abort in strict mode. `E_SIZE` -> `ResultInsufficientBuffer`.

Startup: `napple_audio_init(&(napple_audio_config){.use_device = 1})` once before the first renderer. Tests use
`use_device = 0`. Environment: `NAPPLE_AUDIO_OFF=1` forces the null sink; `NAPPLE_AUDIO_PREFILL_MS` overrides prefill; `NAPPLE_AUDIO_STATS=1` logs each renderer's render stats
(`napple_audren_get_stats`: render time, over-budget and late frames, dropped voice-frames) every 15 s and at close, and each effect type once when
it is first used.

### 1.2 Output path and latency

Renderer frame = `sample_count` samples (240 at 48 kHz = 5 ms) mixed in float; pushed into a lock-free per-source
ring; a single Core Audio unit sums all sources (audren renderers, audout streams), resampling sources whose rate
is not 48 kHz, and applies the master volume. Output starts after `prefill_ms` (default 20 ms = 4 frames) and again
after an underrun; after a device stall the backlog is trimmed back to the prefill, so latency does not accumulate.
`napple_audio_get_stats` reports underruns, overruns, trims and the measured latency.

### 1.3 audren coverage

REV1..REV15 headers accepted (REV5..15 are the tested range). Voices: PCM8/16/24/32/float, DSP-ADPCM; pitch with
linear interpolation; Q14 (<=REV14) and float (REV15) per-voice biquads; wave-buffer state machine (start/stop/
pause/flush, loop, end-of-stream, empty buffers); channel resources; splitters (REV11+, dest stride 0x70/0x90/0xa8
by revision); mixes and submixes (dirty-only mix updates); effects: buffer mix, aux, delay, reverb, reverb3d,
biquad, limiter, capture, compressor; sinks: device (5.1 -> stereo downmix) and circular; voice drop is not
implemented (never drops). Performance metrics are modelled (fixed per-voice/mix/sink costs) into the perf buffer when one is supplied; the layout is unverified.

## 2. input

### 2.1 HID shared memory

`hid:` GetSharedMemoryHandle -> the runtime owns a 0x40000-byte page-aligned block, maps it into the guest and passes
the same pointer as `napple_input_config.shm`. `napple_input_start` then keeps it up to date (120 Hz). The writer
functions (`napple_hid_*`) are pure and can also be called by the runtime directly (e.g. to push a sample inside
ActivateNpad). The layout is table driven (`napple_hid_layout`, profiles `LATEST` and `COMPAT`). See
section 2.4 for which offsets are verified.

| hid IPC | Call |
|---|---|
| SetSupportedNpadStyleSet | `napple_input_set_supported_styles(in, mask)` |
| SetNpadJoyAssignmentModeSingle/Dual | `napple_input_set_assignment_mode` |
| SetSupportedNpadIdType / ActivateNpad | `napple_input_set_player_count(in, n)` |
| GetNpadStyleSet / GetNpadJoyAssignment | read the shared memory entry (the backend keeps it current) |
| SendVibrationValue(s) | `napple_input_set_vibration(in, npad_id, side, &v)` |
| StartSixAxisSensor / StopSixAxisSensor | `napple_input_set_sixaxis_enabled` |
| SendVibrationValue (201) / SendVibrationValues (206) | handle = bytes {type, npad style index, npad id, device 0 left / 1 right}; `napple_input_set_vibration(in, npad_id, side, &v)` (clamped, remembered, played through CoreHaptics) |
| GetActualVibrationValue (202) / GetVibrationDeviceInfo (200) | `napple_input_get_vibration` / type 1 left, 2 right LRA |
| ActivateTouchScreen / Mouse / Keyboard | nothing to do: always written |

### 2.2 Controllers

Style reported per slot: Handheld in slot 9 (npad id 0x20) always; player 1 (id 0) reports ProController when a
pad is attached (or JoyDual when the game supports only JoyDual/JoyLeft/JoyRight), otherwise the keyboard acts as a
Pro Controller. Players 2..8 map to additional GameController devices. GameController families are mapped by
position (Xbox/PlayStation: south = Switch B) or by label (Nintendo pads), automatic via
`GCController.productCategory`. Triggers become ZL/ZR (digital at 50%). Touchpad and gyro of DualSense/Switch Pro
feed the six-axis sensor.

### 2.2b Remapping, analog tuning, style and console mode (input.json, per game)

`napple_bindings` (JSON keys in brackets; every key optional, `version` stays 1, unknown keys are ignored, a file that fails validation is kept and defaults are used):
two keyboard keys per Switch button (`keys`), the four keys of each stick (`stick_left`, `stick_right`), one controller element per button (`pad`), radial dead zone
(`deadzone`, 0..0.9), outer range (`stick_range`, 0.5..1: the raw deflection that counts as full, for worn sticks; dead zone must stay below it), response curve (`stick_curve`,
0.5..3, exponent after the dead zone, >1 = finer near the centre), trigger press point (`trigger_threshold`, 0.05..0.95, replaces GameController's fixed 0.5 for ZL/ZR),
axis inversion (`invert` = [left X, left Y, right X, right Y], applied to pads and keyboard sticks), face button layout (`face_layout`), mouse as touch screen (`mouse_touch`),
the npad style reported to the game (`style`: auto, pro, joycon, handheld; honoured only when the game's SetSupportedNpadStyleSet includes it, else the automatic order
FullKey, JoyDual, Handheld) and the console mode (`operation_mode`: docked 1080p / handheld 720p; read once at start by `services.c`, `NAPPLE_OP_MODE` overrides).
Diagonal keyboard stick input is normalised to the unit circle. `napple_input_keys()` presses several keys atomically (chords such as L+R for the title of Link's Awakening).
Controller Home / Guide and Share / Capture fire `napple_input_set_hotkey_callback` (the frontend opens the overlay / takes a screenshot); keyboard F1 and F12 do the same.
The overlay's Controls tab edits all of the above and lists the connected controllers; Esc cancels a pending capture instead of closing the menu.
**Per-player profiles** (`napple_input_get/set/clear_player_bindings`, files `input-p<N>.json` next to `input.json`): a player with a profile maps its keyboard (player 1) and controller input with its own bindings, the others follow the shared set. The Controls tab lists the players, shows a live controller diagram (`napple_input_get_pad_state`) and edits the selected player's profile.

### The overlay panel (frontend/src/overlay.m, simulators.m)
A floating glass panel pinned to the top right, full height with a 16 pt margin, (`NSGlassEffectView` on macOS 26, a vibrant material before), toggled by F1, Cmd+, or the controller Home / Guide button. Game input keeps flowing while it is open (`panel_passthrough`, default on); a panel opened with the controller sets `overlayPadMode`: the game sees neutral input and the pad navigates the panel through GameController (D-pad moves and adjusts, A activates, B closes, LB/RB switch tabs, hold repeats). Tabs: **Audio/Video** (volume, play audio in the background, window, render target 0.5x/1x/2x/4x, supersampling, upscaler, output filter, sharpness, anisotropy, cursor, statistics: corner, size, background, native Metal HUD via `CAMetalLayer.developerHUDProperties`), **FPS/Speed** (display rate, vsync, frame generation, game speed, turbo, hot keys), **Controls** (players, profiles, bindings, sticks, console mode), **Game/System** (update and DLC, Mii note, amiibo picker, IR camera, mods and cheats toggles saved in `mods-enabled.json`; the runtime does not load mods or cheats yet).

### 2.3 Settings files

`~/Library/Application Support/Napple/<titleid>/input.json` (bindings, versioned), written atomically.
Saves belong to the runtime: **`~/Library/Application Support/Napple/<titleid>/save/<user>/...`**; the frontend and
input libraries never touch `save/`. `settings.json` is the frontend's.

### 2.4 Layout confidence

Verified by structure checks (sizes close exactly): ring header 0x20, atomic storage 8-byte outer sampling number,
touch screen entry 0x3000, keyboard 0x3D8 of 0x400, Npad entry 0x5000, SixAxisState 0x60 (from a guest SDK).
Reconstructed from public documentation, not yet validated against a running title: offsets of the Npad entry
fields past the first lifo (`napple_hid_layout_for`). If a game reads garbage controller state, edit the table; a
`NAPPLE_HID_DUMP=<path>` hook is provided by `napple_input_update` to dump the block for diffing.

## 3. frontend

Startup sequence:

```c
napple_frontend_config c = {.title_id = "01007EF00011E000", .title_name = "Game", .input = in,
    .callbacks = {.user = app, .on_ready = ready, .on_restart_guest = restart, .on_settings_changed = changed}};
napple_frontend *fe = napple_frontend_create(&c);
napple_frontend_set_manifest(fe, &manifest);     // DLC list + update versions, from the game's data dir
return napple_frontend_run(fe);                  // main thread; from on_ready start guest + gfx
```

* `napple_frontend_metal_layer(fe)` is the `CAMetalLayer*` for `napple_gfx_config.metal_layer`.
* Settings (`napple_settings`) live in `settings.json`; gfx reads `internal_*`, `output_*`, `fps_cap`, `vsync`,
  `fullscreen` at start and on `on_settings_changed`. `napple_frame_limiter_*` is the shared pacer.
* Overlay: **F1** or **Cmd+,** toggles it. Tabs: Video, Audio, Controls (rebind keyboard/controller), Game
  (DLC check boxes, update version picker, "Apply and restart game" -> `on_restart_guest` with the chosen
  `update_version` / `disabled_dlc`). The runtime restarts the guest (or the process) and mounts accordingly.
* swkbd: the applet-level parsing (KeyboardConfig, result buffers) stays in the runtime; it calls
  `napple_frontend_text_input_blocking(fe, &req, out, cap)` and converts with `napple_utf8_to_utf16`.
* Focus: on focus loss the frontend releases all input, pauses audio when `mute_in_background` is set and calls
  `on_focus`. Screenshot hotkey **F12** (writes `~/Pictures/Napple/<title>-<date>.png`; `on_screenshot` lets gfx
  provide the frame, otherwise `screencapture` captures the window). Fullscreen: Ctrl+Cmd+F or Cmd+Return.
* App menu: About, Settings (overlay), Fullscreen, Screenshot, Quit (Cmd+Q -> `on_quit`).

## 4. What the runtime must provide

1. Horizon events for `frame_event` / `buffer_event`; session objects for the services in the tables.
2. The HID shared memory block (page aligned) and its handle.
3. The restart action behind `on_restart_guest` and the DLC/update mounts.
4. `NSApplication` is created by `napple_frontend_run`; do not create another event loop on the main thread.

### iOS settings panel (frontend/src/ios/panel_ios.m)
Same four tabs as the Mac overlay, built for touch: a glass panel (UIGlassEffect on iOS 26) pinned to the trailing edge inside the safe area, 52 pt rows, native menus for choices, sliders with live values, haptic ticks, swipe right to close. The game and the on-screen controller stay usable beside it; a panel opened with a physical controller's Home button takes the pad (D-pad moves and adjusts, A activates, B closes, LB/RB switch tabs). Per-player profiles, button capture, stats options (corner, size, background), the Metal HUD, amiibo search and the IR camera (`sim_shared.m`), mods and cheats toggles are all there. Test aid: `SIMCTL_CHILD_NAPPLE_FE_OVERLAY=<tab> xcrun simctl launch ...` opens it on a tab after 6 s.
