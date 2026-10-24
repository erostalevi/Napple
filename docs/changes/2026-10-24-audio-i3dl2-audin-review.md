# Audio: I3DL2 reverb fixes, audin, audren review (2026-10-24)

## Objective
Four goals: (1) an I3DL2 reverb with impulse-response tests, (2) `audin:u` with a capture path that is silent without a device, (3) review the
effects and renderer against audren behaviour (voice drop, underruns, performance metrics, submix order, splitters), (4) keep all of it testable on
Linux. Scope: `audio/` only, plus the one allowed `runtime/` edit (registering `audin:u`, see below).

## Read this first: what was already there
- **The I3DL2 reverb already exists.** `audio/src/reverb.{c,h}` *is* it: effect type 5, `Reverb3dParam` is the I3DL2 parameter set (`room_hf`,
  `hf_reference`, `decay_time`, `hf_decay_ratio`, `diffusion`, `density`, ...). The "plain" reverb is the one in `effects.c` (type 4). Nothing new was
  added as a separate effect; the work was testing the existing one against analytic impulse responses, which found three real bugs (below).
- `audin:u` was not registered at all (the games request it; the 2026-10-23 cloud-session note flags the missing microphone entitlement).

## Bugs found and fixed (each has a test that fails without the fix)
| # | Where | What was wrong | Effect |
|---|---|---|---|
| 1 | `reverb.c` `delay_update` | The delay rings have `cap + 1` slots (a delay of `cap` needs them) and `delay_tap`/`delay_set` use that, but the read/write cursors wrapped at `cap`. Slot `cap` kept the first sample ever written and every tap that crossed the wrap read it. | The first input sample replayed every 400 ms (the pre-delay length) for ever; the tail of the I3DL2 reverb decayed ~3x too slowly (0.5 s decay measured 1.7 s); the four full-length allpass lines were one sample short. Now measured RT60 is within 20% of `decay_time` for 0.5/1/2 s on 1, 2, 4 channels. |
| 2 | `reverb.c` 6-channel output | The centre channel did `output += ...`, every other channel `output = ...`. The mixer runs effects in place, so the dry input stayed in the centre channel at unity gain whatever `dry_gain` was. | Centre channel could not be dried out; out-of-place use added to whatever the buffer held. |
| 3 | `mix.c` submix order | Submixes were ordered by depth along `dest_mix_id` only. A submix routed through a splitter has no `dest_mix_id`, so its real destination was ignored; if the destination had the lower id it was folded first. Mix buffers are cleared every frame, so that audio was **lost**, not delayed. | Now a topological sort over `dest_mix_id` and splitter destinations (lowest id first among the ready ones; a feedback cycle is logged once and its members run last in id order). |
| 4 | `mix.c`, `effects.c` | A rejected effect parameter block (decay 0, delay > delay_max, compressor ratio 0, bad early mode, ...) called `napple_audio_fatal` in the render path, which marks the renderer faulted and mutes it for good. | Parameters are checked before the DSP is created (`audio_reverb_valid`, `audio_effect_valid`); a rejected effect is bypassed and logged once. Buffer-index violations are still protocol errors. |
| 5 | `audren.c` | `voice_drop_enabled`, `clear_voice_drop` and the `voice_drop_count` output existed on the wire but nothing ever dropped a voice, so the count was always 0. | Implemented, see "Voice drop". |

Checked and left alone: the standard reverb (type 4) decays correctly (2 s setting measures 1.6-2.4 s in all four modes, new test). The delay effect's
echoes land at `delay - 1` samples (479 for 480). `effects.c` documents that as the reference line's wrap; I could not verify it against hardware, so
it is pinned by a test with a +-1 tolerance rather than changed.

## Voice drop and render metrics (audren)
- Follows the client's `voice_drop_enabled`. A frame that takes longer than the time limit (`napple_audren_set_time_limit`, percent of the frame
  period) makes the lowest-priority playing voices (highest priority number, then sort order, then id) skip mixing on the following frames: as many
  more as the overrun fraction of the playing voices, at least one. One voice comes back after each frame under 75% of the limit.
- A dropped voice is not mixed but its clock still runs, so its wave buffers are returned on schedule. The number of frames each voice was dropped is
  reported in its update output (`voice_drop_count`) until `clear_voice_drop`.
- **Judgement call:** the trigger is the *measured host render time*, not a model of DSP cycles. I do not have the console's per-command cost table, and
  inventing one would drop voices in games that play fine. On a normal Mac the render takes a small fraction of the 5 ms frame, so nothing is dropped
  in practice; the mechanism exists for a genuinely overloaded host and for games that set a low time limit.
- `napple_audren_get_stats`: frames, total/max/last render ns, over-budget frames, late frames (longer than the whole period), render-thread late
  ticks and resyncs, dropped voice-frames. `napple_audren_set_clock` injects the clock, so the tests are deterministic.
- **Not done: the performance buffer of the update protocol.** `RequestUpdate` still writes a zeroed 0x10-byte performance section. Games that set
  `performance_frame_count` get no per-frame data. I did not trust my memory of the entry layout enough to write one; the host-side stats above are
  what exists. Say so if a game turns out to read it.

## audin
- `audio/src/audin.c` and the `napple_audin_*` API in `napple_audio.h`, modelled on audout: open (default 48000 Hz stereo; 1-2 channels, 8-96 kHz),
  start/stop/state, append, get_released, contains, buffer_count, flush, device gain, stats, manual mode for tests. A pacing thread fills the oldest
  queued buffer in 5 ms slices and releases it, with `buffer_event`, when it is full.
- **Capture path:** one process-wide source, `napple_audin_set_source(read, user)`, giving mono float at the stream rate. **With no source, or when it
  runs short, the stream is silence** (counted in stats). The device is always listed (`BuiltInHeadset`), like a console with no headset.
  Nothing in this tree opens a capture device, so no microphone permission is ever requested and `NSMicrophoneUsageDescription` is still not needed.
- Runtime: `audin:u` (`ListAudioIns`, `OpenAudioIn`, the `Auto` variants and `ListAudioInsAutoFiltered`) and `IAudioIn` (state, start, stop, append,
  buffer event, released, contains, count, gain get/set, flush). Not implemented: `OpenAudioInProtocolSpecified`, the UAC append commands (7, 10).
  `AppendAudioInBuffer` records `data size` bytes, or the whole capacity when the client left `data size` at 0: that rule is my reading of the SDK
  and is **unverified against a game**.

## The `runtime/` edit (as allowed)
`runtime/src/services/audio.c` only: the `AudIn` object and command tables next to `audout`, and one `ipc_register_service("audin:u", ...)` line.
It compiles clean as `clang -fsyntax-only` against the real headers on Linux; the runtime itself needs a Mac to build and link, so it has **not
been built or run**.

## Linux testing
`make -f Makefile.linux test` (clang, `-Wall -Wextra -Werror`, needs `libopus-dev`) builds the whole suite with the sources untouched.
`audio/tests/linux/include` has inert stand-ins for `AudioToolbox`, `CoreAudio` and `pthread/qos.h`; every Core Audio call fails, which `engine.c`
already treats as "no device". Nothing Linux-specific was added to `src/`. The Mac `Makefile` gained the new suites in `test`, `tsan`, `asan`.

| Suite | Checks |
|---|---|
| audren, effects, audout, hwopus, engine, threads, speed | unchanged, all pass |
| reverb3d (new) | analytic early-reflection response, no replay, RT60, dry path and in-place/out-of-place, linearity, stability, validation |
| review (new) | submix/splitter order, routing cycle, voice drop, stats, rejected parameters, delay feedback, standard-reverb decay |
| audin (new) | parameters, silence, sample-exact capture, gain/clipping, shortfalls, queue/flush, translate and write hooks, pacing thread |

Run here: the full suite with clang; ASan+UBSan over all suites and TSan over audin/review/threads/audout with gcc (clang's sanitizer runtimes are
not installed in this container). Clean, apart from two defects in my own test code that I fixed. I also ran each fix's test against the unfixed code
to confirm it fails.

## Needs a Mac (not verified)
- Nothing in the Core Audio path (`engine.c`) was changed, so `make test` on the Mac should behave as before, but it was **not run**: please run
  `make -C audio test` and `make -C audio asan tsan`.
- Build and link the runtime (`audio.c` changes) and check a game that opens `audin:u` (the logs say `audin opened: ... (silence)`).
- The I3DL2 reverb tail is now shorter (it decayed ~3x too slowly before) and matches `decay_time`. Please listen to a game that uses effect type 5 and spot-check for glitches; the first
  sample no longer replays, but the gain of the late tail relative to the dry signal is unchanged and unchecked against hardware.
- Reverb delay-line fix changes the timing of the full-length lines by one sample (the density-100 FDN lines and the allpasses). No hardware reference
  to compare with.
- Real microphone capture (a Core Audio input unit feeding `napple_audin_set_source`, plus the Info.plist usage string) is not written.

## Not done / open
- Performance buffer in `RequestUpdate` (above).
- Docs not updated (outside my scope): `docs/runtime-services.md` line 44 and `docs/audio-input-frontend-interface.md` should list `audin:u`.
- Stereo/quad channel mapping of the standard reverb and the 5.1 reorder (`{0,1,4,5,2,3}`) were read but not verified against hardware.

## Mac verification (2026-10-24, Apple Silicon, macOS 27)
Run on the branch on a Mac by a second session. Audio was judged from logs only: nobody listened.

**Passed**
- `make -C audio test`: all 10 suites, first real Core Audio runs of `test_engine` and `test_threads` with these changes (audren 3410 checks, effects 1253,
  reverb3d 54, review 82, audin 50, audout 506, hwopus 22, engine 11, threads 2, speed ok). `make asan` (ASan+UBSan, 9 suites) and `make tsan` (7 suites):
  0 failures, 0 sanitizer reports (binaries also rerun directly so no report was hidden by the `tail` in the recipe).
- `make -C runtime` compiles and links with no warnings; `make -C runtime test`: ipc 27, fs_journal 16, hid_service 10, timescale 21, all pass.
- audin IPC layout, checked by a harness that includes `runtime/src/services/audio.c` and calls the handlers with hand-built `IpcCall`s (19 checks,
  kept outside the tree because `runtime/tests` is not in this branch's scope): ListAudioIns(Auto) writes "BuiltInHeadset", count 1; OpenAudioIn reads
  {u32 rate, u16 channels} and replies 16 bytes {48000, 2, 2 (PCM16), 1 (stopped)} plus an IAudioIn; bad parameters refused; RegisterBufferEvent copies
  a handle; gain round-trips as float; AppendAudioInBuffer through an A buffer and through an X buffer (Auto); data size honoured, size 0 fills the
  whole capacity; GetReleasedAudioInBuffer(sAuto) through B and C buffers return the tags in order and zero the unused ones; flush releases. The log
  line `audin opened: 48000 Hz, 2 channels (silence)` is printed. IPC macro use matches audout (`IPC_IN` for the float, `ipc_out` for the 4-word reply).
- Super Mario Odyssey, windowed, real Core Audio device, scripted input (title, Start, opening skipped, then moving, jumping and cap throws), 430 s
  (1.5x game speed until 98 s): renderer 96 voices / 14 effects / 3 submixes, REV3, voice drop **on**, time limit 100%. Over 90,000 frames:
  **0 dropped voice-frames, 0 over-budget, 0 late frames, 0 late ticks, 0 resyncs**; render time avg 12 us at the title, 50-57 us in play, max
  527 us (budget 5 ms). Device output: **0 underruns, 0 overruns, 0 trims** in every 15 s line; frames requested = wall time x 48 kHz; latency
  5.3-24.9 ms. The only effect type in use is 2 (aux); SMO does not use the I3DL2 reverb (type 5) and does not request `audin:u` in this run.

**Fixed here**
- `audin.c`: the 32-buffer limit counted only queued buffers. With 32 released but not yet collected, a 33rd append was accepted and, once full,
  its tag fell off the 32-slot released list: the guest would wait for it for ever. The limit now counts queued + released-uncollected (what
  GetAudioInBufferCount reports). Test in `test_audin.c` (fails without the fix).
- `audio/Makefile`: `asan` / `tsan` piped each suite through `tail`, so a failing suite still exited 0. Now `set -o pipefail` (checked: with the bug
  reinjected `make asan` exits 2).
- Added `NAPPLE_AUDIO_STATS=1` (library only): each renderer logs `napple_audren_get_stats` every 15 s of frames and at close, and "effect type N in
  use" once per type. This is how the numbers above were taken; there was no way to see the stats from a game before.

**Not verified / still open**
- `audin:u` in a real game: neither available game opened it (SMO: not in 430 s; Link's Awakening: aborts at 1.1 s with fs result 0x35f202
  (module 2, desc 6905) before any audin request, identically with the `main` runtime, so it is an existing fs issue, not this branch). The
  size-or-capacity rule is therefore still unverified against a game.
- I3DL2 reverb (type 5) by ear: no game here that uses it was runnable, and no listening was done. Only the impulse-response tests cover it.
- Performance buffer of `RequestUpdate` still zeroed; real microphone capture still not written (known gaps, unchanged).
