---
branch: claude/media-live-2026-11-01-r1
orchestration: media-live
base: df2f318
head: d8ba88f
env: mac
date: 2026-11-01
areas: [runtime/services (nvdrv media, caps), runtime/movie]
status: integrated
---

# claude/media-live-2026-11-01-r1: verify video, movie audio and album JPEG on real titles

## Summary
All four acceptance items are verified on this Mac with the shared title data (headless, audio device off, one run at a time under `.run.lock`):
- **H.264 via nvhost-nvdec + VIC (MK8D):** 300 NVDEC decodes / 300 VIC frames over two channel opens, 0 errors; the dumped VIC frames are clean
  pictures and the Grand Prix panel plays them on screen.
- **VP9 + Vorbis via the movie library (LAS):** the opening movie (VP9 1920x1080 in NVN texture mode on libvpx, Vorbis 48 kHz 6 ch on stb_vorbis, out-of-band
  headers, default on): 6600 frames, 0 errors. The Vorbis PCM is real signal (new level lines). SMO and ACNH boot unchanged with movie audio on.
- **JPEG album round trip:** new e2e test through caps:su -> caps:u -> accessor session -> caps:dc and ImageIO, run on real MK8D frames.
- **nvdisp-disp0:** no title of the four opens it. The new device-open summary notes show MK8D opens nvhost-ctrl/nvmap/nvhost-nvdec/nvhost-vic only,
  and LAS/SMO/ACNH open no device at all.
One real fix: the engine class's THI word 0 (INCR_SYNCPT) and two real registers (VIC 0x1c1, NVDEC 0x140) were counted as unknown, which put false
"writes to registers outside the known map" notes in every MK8D summary.
Not done: AAC (no title creates a type 4 decoder, so no hookup), nvjpg/msenc/nvdisp on a title (none opens them), VP9 through NVDEC.

## Files
`git diff --stat df2f318..HEAD` (before this file):
- `runtime/src/services/nvdrv_media_core.c` (+67): `NAPPLE_VIC_DUMP` frame dump, THI INCR_SYNCPT on engine classes, known-register additions, engine creation sequence.
- `runtime/src/services/nvdrv.c` (+24): `open_note` / `disp_note`: first Open of each path and first call of each nvdisp ioctl nr go to the log and summary.
- `runtime/src/services/nvdrv_media_regs.h` (+2): `NVDEC_SET_UNNAMED_0x140`.
- `runtime/src/movie.c` (+29): Vorbis level lines (`pcm_level`), `#include <math.h>`.
- `runtime/tests/test_svc_nvdrv_media.c` (+7): 3 checks. New: `runtime/tests/test_svc_caps_e2e.c` (picked up by `test-services` automatically).
- Docs: new `docs/archive/changes/2026-11-01-media-live-2026-11-01-r1.md`. Shared, append-only: `docs/architecture/runtime-services.md` (new
  section at the end), `docs/reference/environment-variables.md` (3 rows added after `NAPPLE_DECODER`).
- Not in the repo: `work` symlink at the worktree root (git-excluded).

## Built and tested here
| Item | Status | Evidence (command + result) |
|---|---|---|
| Acceptance 1: `make -C runtime test` + media tests | tested | `make -C runtime test && make -C runtime test-services` rc 0, every test `"failed":0` (svc_nvdrv_media 172, svc_caps_e2e 22, svc_caps 146, svc_caps_dc 13, svc_caps_image 100, svc_jpeg 43, svc_movie_audio 43, svc_codec_headers 15; test_decoder_vt / test_media green) |
| Acceptance 2: real title decodes H.264 through NVDEC/VIC | **verified** | MK8D, `NAPPLE_MEDIA_TRACE=1 NAPPLE_VIC_DUMP=… NAPPLE_INPUT_SCRIPT="15:A,19:A,23:A,27:A,31:A,35:A" tools/boot_test.sh 0100152000022000 110` -> exit 0; `[media] class 0xf0: 180 executes, 179 frames, 0 errors` + `120/119/0` (second channel), VIC the same; frame PNG `mk8d-vic-frame120.png` and the in-game panel `mk8d-grandprix-panel.png` in the shared notes folder `media-live-evidence/` |
| Acceptance 2 (VP9 movie path) | **verified** | LAS 115 s: `movie: VP9 decoder … on the host (v1.17.0), mode 2 (NVN texture)`, `6600 frames, decode 0.80 ms per frame`, 0 errors; `las-opening-movie.png` |
| Acceptance 3: movie audio default on, LAS + SMO no regression | **verified** | LAS: `Vorbis headers out of band (2 csd blobs): complete`, `audout opened: 48000 Hz, 6 channels`, `Vorbis: 2000 packets, 37.6 s of 6-channel PCM; … RMS 2399, 7908 of 2886528 samples at full scale`; SMO 115 s exit 0, no crash (creates no movie decoder); ACNH 90 s exit 0 |
| Acceptance 4: JPEG album save/load round trip | **verified (e2e, real frames)** | `NAPPLE_CAPS_E2E_PNG=<MK8D screenshot> runtime/build/tests/test_svc_caps_e2e` -> 23 checks, 0 failed; PSNR caps:dc 31.8-39.9 dB, ImageIO 32.5-41.1 dB, caps:dc vs ImageIO 38-45 dB, ImageIO JPEG through caps:dc 32.9-41.2 dB. Not title-driven: no title of the four opens caps |
| nvdisp-disp0 vblank ioctls on a title | **verified: none of the four opens it** | run summaries: MK8D `nvdrv: opened /dev/nvhost-ctrl|nvhost-nvdec|nvmap|nvhost-vic`; LAS/SMO/ACNH only nvdrv cmds 3, 8, 13 (no Open) |
| THI INCR_SYNCPT / 0x1c1 / 0x140 known | verified + tested | final MK8D run (17:24) summary has only the 4 open notes (before: 543 / 382 false unknown writes); 3 new unit checks |
| nvjpg, msenc, VP9-on-NVDEC, RGB / block-linear VIC | tested only | no title of the four uses them |

## Unbuilt / untested list (the integrator checks these first)
None: everything was compiled and run on this Mac. `disp_note` (nvdisp first-ioctl note) only ran in the unit test (`test_svc_nvdrv_media` prints it), since no title opens nvdisp.

## Findings
- **No title of the four opens nvdisp, nvjpg, msenc or caps.** LAS, SMO and ACNH never Open an nvdrv device (NVN is HLE). Those rows can only rise
  through tests unless another title is added to the test material.
- **False unknown-register notes (fixed):** MK8D's NVDEC/VIC submits end with a THI INCR_SYNCPT (engine-class word 0). VIC 0x1c1 SetControlParams
  was named in b7 but left out of `known_reg`. NVDEC 0x140 carries a constant buffer address (0x100210 in device-page units). Its name is unknown, so I
  called it `NVDEC_SET_UNNAMED_0x140` instead of guessing.
- **LAS movie audio peaks at full scale** in 0.01-0.27 % of samples. The float->s16 clamp in `movie_audio_core.c` is exact, so this is stb_vorbis output beyond
  +-1.0 (hot mastering). It is not a bug I can show; a reference decode (ffmpeg) of the same packets would settle it.
- LAS's title screen asks for L+R, so the golden route `boot-to-village` (A presses only) stays on the title screen and loops the opening movie. A
  `tools/golden/routes.json` issue (not mine): the route probably needs `L+R`.
- Decision: the round-trip check is a unit-level e2e (`test_svc_caps_e2e`) fed with real game frames, because no title drives caps. The acceptance allows "run or e2e".
- Decision: `MIN_DB=26` / `AGREE_DB=33` in the e2e test leave about 6 dB of headroom below the noisiest MK8D frame (sparkle backgrounds: 31.8 dB). A layout or
  channel bug falls under 15 dB.
- `cargo` is not on PATH in these session shells. Use `export PATH="$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/bin:$PATH"` (noted in the shared notes folder).

## How to integrate
- No merge-order constraints (depends_on: none). Touches only media-live's owned paths plus two append-only doc edits.
- Conflict hotspots: `docs/architecture/runtime-services.md` and `docs/reference/environment-variables.md` (other branches append there too: keep both
  sides). `runtime/src/services/nvdrv.c` could conflict with a branch that touches nvdrv's GPU nodes (sweep-small?). My hunks are two new static functions above
  `nv_open`, one call after the `LOGV` in `nv_open`, and one call in the `DEV_DISP` case of the ioctl switch.
- Build: `make -C gfx lib adapter && make -C frontend && make -C input && make -C audio && make -C runtime` (no errors), then
  `make -C runtime test && make -C runtime test-services` (all `"failed":0`).
- No Linux-only calls. `test_svc_caps_e2e` keeps its ImageIO parts under `#ifdef __APPLE__` (the Linux check runs the caps-only half).

## How to test
1. `make -C runtime test test-services`: passing = rc 0 and `{"test":"svc_caps_e2e","checked":22,"failed":0}`, `svc_nvdrv_media … 172 … 0`.
2. Real frame e2e: `NAPPLE_CAPS_E2E_PNG=<any 1280x720 game screenshot> NAPPLE_CAPS_E2E_OUT=/tmp/x runtime/build/tests/test_svc_caps_e2e`, which prints
   PSNR lines above 26 dB and 23 checks, 0 failed. `/tmp/x/album.jpg` opens in Preview.
3. MK8D: `NAPPLE_AUDIO_DEVICE=0 NAPPLE_MEDIA_TRACE=1 NAPPLE_VIC_DUMP=/tmp/v NAPPLE_INPUT_SCRIPT="15:A,19:A,23:A,27:A,31:A,35:A" lockf -k /Users/neto/NappleWork/.run.lock tools/boot_test.sh 0100152000022000 80`.
   Pass: `[media] class 0xf0: 120 executes, 119 frames, 0 errors`, `/tmp/v-<n>-1.ppm` files (open with Preview), and in
   `work/compat/0100152000022000/*/runtime-boot.json` notes the four `nvdrv: opened …` lines and **no** "outside the known map" line. Fail: errors > 0,
   green/grey frames, or unknown-register notes.
4. LAS: `NAPPLE_AUDIO_DEVICE=0 lockf -k /Users/neto/NappleWork/.run.lock tools/boot_test.sh 01006BB00C6F0000 45`. Pass: `movie: Vorbis: 500 packets, 10.0 s of 6-channel PCM; … RMS 18xx`
   and `movie: NNNN frames, decode ~0.8 ms per frame`, no `decode error`.
5. Regression guard: SMO 60-115 s boot (exit 0, no crash). nvdrv GPU-node tests (`test_svc_nvdrv_nodes` if present) must stay green, because nvdrv.c changed.
6. Human (optional): MK8D main menu with sound on. The Grand Prix / Time Trials panels play their preview video. LAS opening movie with sound.

## Fix policy hints
- Safe to fix directly: doc row wording, include order, a renamed helper in nvdrv.c on conflict.
- If a merged branch changes `media_engine_new` / `MediaEngine`, keep the `seq` field (it only names the dump files).
- Needs a decision: if 26 dB proves flaky on some other real frame, lower `MIN_DB` (a real fault lands below 15 dB) rather than dropping the real-frame mode.

## Needs from other areas
- `tools/golden/routes.json` (not owned): LAS `boot-to-village` needs `L+R` at the title screen ("Press L+R"); with A only it never leaves the opening movie.
- Coverage: nvdisp / nvjpg / msenc / caps can only be verified by a title that uses them; none of the four test titles does.

## Integration result
- Date: 2026-11-01. Merged into local `main` as f557b4a (`--no-ff` merge of a2b5a21, on top of sweep-small). Outcome: **Clean** (no `integrate:` fixes, no native ports; nothing Linux-only in the diff). The expected `nvdrv.c` / doc conflict hotspots did not occur (sweep-small touched neither).
- Tests on this Mac (scratch worktree, same tree as the merge): gfx lib/adapter, audio, input, frontend, runtime build clean; `make -C audio test`, `make -C input test`, `make -C runtime test` (test_media, test_decoder_vt green) and `make -C runtime test-services` (60 service tests, 0 failed: svc_caps_e2e 22, svc_nvdrv_media 172, svc_nvdrv_nodes 105, svc_bcat 137) all rc 0.
- Real frame e2e: `NAPPLE_CAPS_E2E_PNG=<SMO 1280x720 frame>` -> 23 checks, 0 failed (caps:dc 47.1 dB, ImageIO 47.4 dB, caps:dc vs ImageIO 55.8 dB).
- MK8D 80 s (How to test 3, run lock, audio device off, copied saves): exit 0; NVDEC class 0xf0 and VIC class 0x5d 60/120/180 executes per channel, **0 errors**; the four `nvdrv: opened` notes and **no** "outside the known map" note in the summary; VIC dumps `v-<n>-{1,60,120}.ppm` are clean pictures (Mario kart preview frame checked by eye).
- LAS 45 s (How to test 4): VP9 mode 2 + Vorbis out-of-band headers complete, `audout opened: 48000 Hz, 6 channels`, `Vorbis: 500 packets, 10.0 s ... RMS 1838`, 900 frames at 0.46-0.76 ms, no decode error.
- Correction to "How to test": `boot_test.sh` writes into the shared `work/saves`; the integration runs used the runtime binary directly with a copied save root.
