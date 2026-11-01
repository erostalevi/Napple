---
branch: claude/sweep-media-r2-2026-11-01-r2
orchestration: sweep-media-r2
base: 79415f0 (orchestrate/2026-11-01-r2)
head: d8b1493
env: mac
date: 2026-11-01
areas: [runtime/movie, runtime/services (movie_audio_core), runtime/main, input, tools/golden, docs]
status: ready
---

# claude/sweep-media-r2-2026-11-01-r2: movie AAC / VP8 / ARGB, clean guest exit, npad device types, LAS golden route, doc rows

## Summary
- **Movie library decoders:** AAC (type 4) on AudioToolbox and VP8 (type 2) on libvpx run on the host. Output format 2 now works. It is ARGB, read
  from the movie library's own `DecoderImpl::ConvertDecoderOutputFormat` / `ConvertToArgb` (disassembled from LAS subsdk1: byte order A R G B,
  BT.709 YUV coefficients on full-range Y). Other non-NV12 video buffer formats are refused, as the library itself does. A new harness test drives
  `movie.c`'s decoder entries with fake guest objects and streams made by encoders: 1015 checks.
- **Clean guest exit:** the SDK 15 `nn::init` finalizer callback now runs every module's finalizers in reverse load order, reports and syncs saves,
  writes the summary and exits 0. A forked-child harness test covers it.
- **npad device types:** the standard styles report libnx HidDeviceTypeBits (decision given by the orchestration). `NAPPLE_NPAD_DEVICE_TYPES=0`
  reverts.
- **LAS `boot-to-village`:** L+R at the title, then file select, the software keyboard name entry, Tarin's house, the shield, and out into **Mabe
  Village**. Verified twice, the second time through `golden.py` itself. It runs 205 s with Metal validation off for this route; the reason is
  under Findings.
- **Docs:** the audren perf, hid styles and bcat rows are refreshed. The audin mic decision (auto with silence fallback, kept) is documented in the
  audin row and the env doc.
- Not done: nothing from the acceptance list. Gaps are only "verified on a title" for AAC / VP8 / format 2, because no test title uses them.

## Files
`git diff --stat 79415f0..d8b1493`: `runtime/src/movie.c`, `runtime/src/services/movie_audio_core.[ch]`, `runtime/src/main.c`,
`input/include/napple_input.h`, `input/src/hid_shm.c`, `input/tests/test_styles.c`, `tools/golden/routes.json` (LAS route only).
New tests: `runtime/tests/test_svc_movie_host.c`, `runtime/tests/test_svc_guest_exit.c`; extended: `runtime/tests/test_svc_movie_audio.c`.
Docs: `docs/archive/changes/2026-11-01-claude-sweep-media-r2-2026-11-01-r2.md` (new). Shared, edited in place (only the assigned rows):
`docs/architecture/runtime-services.md` (audren:u, bcat:u, hid, audin:u rows of the small-gaps table) and `docs/reference/environment-variables.md`
(`NAPPLE_AUDIN_SOURCE` row corrected; `NAPPLE_NPAD_DEVICE_TYPES` and `NAPPLE_MOVIE_AAC` rows added under it). Nothing deleted.

## Built and tested here
| Item | Status | Evidence (command + result) |
|---|---|---|
| Acceptance 1: AAC (type 4) via AudioToolbox | tested (encoder-made streams) | `runtime/build/tests/test_svc_movie_host`. AAC stereo csd-0: 98304 frames for 96000 in, tones 440.0 / 1000.0 Hz, RMS 11581 / 11554 (11585 in). The same with ADTS framing and with no csd-0. 5.1 per-channel Hz `300 500 700 60 1100 1300` = WAVE order L R C LFE Ls Rs. Mutation (no remap) gives `700 300 500 1100 1300 60` and fails |
| Acceptance 1: VP8 (type 2) via libvpx | tested | same test: VP8 NV12 worst PSNR 51.6 dB over 30 frames, end of stream, no error event |
| Acceptance 1: non-NV12 output formats | tested; format 2 implemented, the rest documented | format 2 = ARGB as ConvertToArgb (VP8 47.9 dB / VP9 44.0 dB against the library formula applied to the source; A = 255). Format 1 is audio only, 3 is texture mode; other formats for video are refused (IsOutputFormatSupported); test checks the refusal |
| ASC / ADTS parsers | tested (portable) | `test_svc_movie_audio` 43 -> 54 checks |
| LAS Vorbis + VP9 movie after the audio refactor | verified | every LAS run: `movie: 5100 frames …`, `Vorbis: 4500 packets, 85.8 s of 6-channel PCM; … RMS 5939` |
| Acceptance 2: guest exit | tested (harness) | `runtime/build/tests/test_svc_guest_exit`: forked child, finalizers ran `S0cba` (sdk DT_FINI, subsdk0, main fini_array backwards; gone NRO and null / -1 entries skipped), exit status 0, summary `partial` / `guest exited normally` / last stage `exit`. 5 checks. No test title returns from nnMain, so not seen on a title |
| Acceptance 3: LAS route with L+R reaching the village | **verified** | direct run x9 (fresh save, 195 s): shield at frame 9700, "Mabe Village" banner at 10700. Then `NAPPLE_AUDIO_DEVICE=0 lockf -k /Users/neto/NappleWork/.run.lock python3 tools/golden/golden.py --golden-root <tmp> run las --route boot-to-village --data-root /Users/neto/NappleWork/games`: PASS, exit 0, 12061 frames, 0 gfx warnings, frame 11200 = Link outdoors in Mabe Village. Frames in the shared notes folder `sweep-media-r2-evidence/` |
| Acceptance 4: npad device types | tested + verified (no regression) | `make -C input test`: styles 49 -> 51 (FullKey 0x1, handheld 0xc, pair 0x30, left 0x10, right 0x20; `NAPPLE_NPAD_DEVICE_TYPES=0` -> 0, retro bits kept). All four titles take input normally with the new bits |
| Acceptance 4: mic default | documented | audin:u row + `NAPPLE_AUDIN_SOURCE` env row: auto (mic when permitted, else silence), `silence` opts out. No code change |
| Acceptance 5: doc rows | done | runtime-services.md audren:u / bcat:u / hid rows (+ audin:u) |
| Acceptance 6: four-title regression | verified | SMO 100 s, MK8D 100 s, ACNH 90 s (exit 0, 0 crashes, screenshots: SMO Start -> intro, MK8D title -> Grand Prix -> cup -> race, ACNH dialogue advancing). LAS: eleven route runs, exit 0 each |
| Suites | tested | `make -C runtime test && make -C runtime test-services` rc 0, no suite with failures. `make -C input test`, `make -C audio test` all 0 failed |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt. `movie_audio_core.c`'s non-Apple AAC stubs (`#else` branch) never compiled here; they are trivial. The Linux harness compiles them
  if it builds `test_svc_movie_audio`.
- AAC HE-AAC / SBR (`kAudioFormatMPEG4AAC_HE` via FormatInfo) is untested: AudioToolbox's encoder here was used for AAC-LC only.
- The output-format metadata for format 2 (`color-format` 16, `stride` = width x 4) follows Android's ARGB convention. The library's own
  GetOutputFormat for format 2 was not read, so this part is proposed.

## Findings
- **DecoderOutputFormat values** (from LAS subsdk1): 4 NV12 (row copy), 2 ARGB (ConvertToArgb +0x1ef90, reached through the PLT: GOT slot 0x5ef338,
  relocation symbol `_ZN5movie11DecoderImpl13ConvertToArgb…`), 1 PCM, 3 texture. ConvertToArgb's constants are at +0x524090 / +0x524158 / +0x52407c /
  +0x52410c. Method: the rewritten `subsdk1.patched.bin` + `.tables` symbols, wrapped in a minimal ELF and run through `objdump`. Helper scripts are
  kept outside the repo.
- The movie library ships Android's **SoftAAC2** (subsdk1), so `NAPPLE_MOVIE_AAC=0` is a real fallback: the library decodes AAC on the CPU itself.
- AudioToolbox's AAC decoder gives the bitstream channel order (C L R Ls Rs LFE) unless asked for a layout. The code asks for the WAVE layout and
  keeps a manual remap as a fallback; both paths were checked.
- **LAS route facts:** the opening movie (~84 s) cannot be skipped on a fresh save; the title needs L+R. The name entry uses the swkbd library applet
  (returns "Link"). Use B in dialogue: A starts talks / checks with whatever Link faces, and that broke two route attempts. Tarin blocks the door
  until you talk to him and get the shield.
- **Decision (route + validation):** walking distance is per frame while script times are seconds, so under Metal validation (~7% slower, the
  golden.py default) the walk to Tarin stops short and the route ends at the door. That golden.py run "passed" only because it had no goldens to
  compare. I set `"validation": false` on this route, a field golden.py already supports. Alternative for a later session: make input_script
  frame-based (not my file).
- **Decision (guest exit status):** a normal exit writes `partial` / "guest exited normally" rather than `pass`. A title quitting says nothing about
  whether it works.
- The route is 205 s, over the batch's 120 s run guideline, because the unskippable movie alone takes 84 s. Tuning took about ten runs of 100-205 s
  under the lock.
- Harness gotchas outside my area: `NAPPLE_SCREENSHOT_FRAMES` stops silently after 20 files (`gfx_glue.c`). `seq` with a decimal step prints commas on
  this Mac (pt-BR locale), so generate input scripts with Python.

## How to integrate
- No merge-order constraints. Conflict hotspots:
  - `docs/architecture/runtime-services.md` rows (audren:u / bcat:u / hid / audin:u in the small-gaps table) and `docs/reference/environment-variables.md`.
    Take both sides; my edits are confined to those rows plus two new env rows.
  - `runtime/src/main.c` (only `cb_finis` and the new block above it).
  - `input/src/hid_shm.c` (`napple_hid_device_type_for`). ipc-depth-hid may touch hid files, but `runtime/src/services/hid*.c` is untouched here.
- Build: `make -C input && make -C audio && make -C gfx lib adapter && make -C frontend && make -C runtime`, then `make -C runtime test test-services`,
  `make -C input test`.
- No Linux-only calls. The AudioToolbox and libvpx-encoder code is Apple-only by design: `movie_audio_core.c` `#if defined(__APPLE__)`, and
  `test_svc_movie_host.c` / `test_svc_guest_exit.c` are Mac tests.
- `test_svc_movie_host.c` `#include`s `movie.c` and renames `movie_register_overrides` / `movie_host_tick` / `module_find_symbol` by macro, so it
  links next to `movie.o`. `test_svc_guest_exit.c` `#include`s `main.c` with `main` renamed and stubs `frontend_gfx_ready` /
  `frontend_wait_shader_prewarm`. If `main.c` gains new app-side dependencies, add stubs there.

## How to test
1. `make -C runtime test test-services`. Pass: rc 0, `{"test":"svc_movie_host","checked":1015,"failed":0}`, `svc_movie_audio 54`,
   `svc_guest_exit 5`. `make -C input test`: styles 51.
2. LAS route (about 3.5 min): `NAPPLE_AUDIO_DEVICE=0 lockf -k /Users/neto/NappleWork/.run.lock python3 tools/golden/golden.py --golden-root /tmp/g run las --route boot-to-village --data-root /Users/neto/NappleWork/games`.
   Pass: PASS, and `<out>/…/frame-11200.png` shows Link outdoors in Mabe Village. Failing looks like Tarin's "Whoa, boy!" box at the door in
   frame 11200, which means the machine was too busy and Link walked short; rerun on a quieter Mac.
3. Regression: SMO / MK8D / ACNH boots with their golden inputs take input normally (device-type bits). `NAPPLE_NPAD_DEVICE_TYPES=0` reverts if a
   title misbehaves.
4. Human (optional): a title with an AAC or VP8 movie, if one is ever added to the test material.

## Fix policy hints
- Safe to fix directly: doc row wording, test stubs for new `main.c` dependencies, include order.
- Needs a decision:
  - If a title shows wrong colours in a format-2 movie, compare against ConvertToArgb's constants first.
  - If a title reads `stride` from the output format, the format-2 metadata (proposed) may need the library's real values.
  - Device types: revert with `NAPPLE_NPAD_DEVICE_TYPES=0` rather than editing the table.

## Needs from other areas
- `runtime/src/kernel.c` `svc_exit_process`: write a clean-exit summary instead of `fail` (titles on the 2-callback `nn::init::Start` end there).
- `runtime/src/services/audio.c`: the OpenAudioIn comment still says the microphone is opt-in.
- `runtime/src/gfx_glue.c`: log when the 20-screenshot cap is hit.
- `runtime/src/input_script.c`: a frame-based time option would make walking routes independent of the frame rate (and of Metal validation).
