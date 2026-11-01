---
branch: claude/sweep-small-2026-11-01-r1
orchestration: sweep-small
base: df2f318
head: 53e508c (code; the integrator file itself is one docs commit on top)
env: mac
date: 2026-11-01
areas: [audio, input, content/napple-core, runtime/services/bcat]
status: ready
---

# claude/sweep-small-2026-11-01-r1: close seven small partial items (audren perf, audin mic, retro pad styles, NSZ, tickets, NCA2/0, bcat storage)

## Summary
Every layout question was settled against public reference code read during the session (libnx `hid.h`, the yuzu-derived `skyline-emu/audio-core`, LibHac BCAT) and against the user's own Testing Material, and two real bugs came out of it:
- **audren performance buffer** had the wrong layouts. It now matches format 1 (REV1..4: header 0x18, entry/detail 0x10) and format 2 (REV5+, so REV11+ too: header 0x30, entry/detail 0x18), PERF magic in both, chain ended by a null header; tests for REV1,3,4,5,7,9,11,12.
- **GC trigger lifo** was at 0x4C20; libnx puts it at **0x4228** (lark/lucia/lager words at 0x43E0). Plus Lucia/Lager type words, retro device-type bits and `HidNpadButton` bits 28..34 (Palma, HandheldLeftB, N64 C buttons).
- **audin** microphone is the default source when the process may use it; denied / not askable / failed start = silence, no failure. A real capture ran here (4800 frames, non-zero RMS).
- **NSZ**: a minimal NCZ writer in a test turns the real SMO NSP into a block NSZ (all four NCAs, incl. the 5.5 GB RomFS one); every NCA reads back SHA-256-identical, and `napple build --data-only` on it gives the same module hashes as the NSP build.
- **Personalized tickets**: the real `eticket_rsa_keypair` of prod.keys (0x210 bytes) confirms the D/N/E layout; ticket built against its public key decrypts. The container.rs wiring already existed (no change needed).
- **bcat**: reads the console's save-data storage layout (`directories.meta` / `files.meta` / `files/`), auto-detected from `NAPPLE_BCAT_DIR`; progress `Done` status fixed 6 -> 9.
Not done: **NCA2 / NCA0 retail validation** (all 39 NCAs in the material are NCA3; stays synthetic-tested); XCZ with a real dump (same NCZ code path, no XCI with NCZ in the material).

## Files
`git diff --stat df2f318..HEAD`: audio (`audren.c`, `audin_mic.c`, `audin_source.c`, `internal.h`, `napple_audio.h`, tests), input (`hid_shm.c`, `input.c`, `napple_input.h`, `test_styles.c`), napple-core (`nca.rs` +`Nca::ctr_sections`, `ticket.rs` comment, new tests `nsz_real.rs`, `ticket_real.rs`, `nca_versions_real.rs`), runtime bcat (`bcat_core.[ch]`, `tests/test_svc_bcat.c`), `docs/archive/changes/2026-11-01-claude-sweep-small-2026-11-01-r1.md`, this file. No shared files touched, nothing deleted.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| audren perf layouts (per revision) | verified (layout from reference code) / tested | `make -C audio test`: audren 3640 checks, 0 failed; layout cross-checked with skyline-emu/audio-core structs |
| audren perf in a headless boot | **not provable with these titles**: 60 s headless boots of SMO (REV3), MK8D (REV1), ACNH (REV15) and LAS (REV7) ran 11,900-12,150 RequestUpdate calls each with no audren error or warning, but none supplies a performance buffer (`performance_frame_count` 0, perf buffer size 0), so the `performance buffer:` log line never fires; the line is proven by `audio/build/test_audren` output only | see "How to test" 2 |
| audin default mic + silent fallback | tested + verified capture | `audio/build/test_audin_source` 52 checks; denied/undetermined/hook-refused leave exact zeros; real capture: `nae_mic_permission()==granted` (Terminal), `install_source_spec("mic")` -> 4800 frames pulled, rms 0.0014 |
| input GC/Palma/NES/SNES/N64 | tested | `make -C input test`: styles 49 checks, all suites 0 failed; offsets from libnx `HidNpadInternalState` |
| NSZ real dump | verified | `NAPPLE_TESTING_MATERIAL=~/Desktop/switchfiles NAPPLE_NSZ_FULL=1 NAPPLE_NSZ_BLOCK=1 cargo test --release -p napple-core --test nsz_real`: 4 NCAs converted, ok in 85 s; `napple build ... smo.nsz --data-only` (partial NSZ in-place, full block NSZ materialize): module sha256 + build id equal to `/Users/neto/NappleWork/games/0100000000010000/3ca12dfaaf9c82da/manifest.json`, main.npdm / control.nacp byte-identical |
| personalized ticket key layout | verified | `cargo test --release -p napple-core --test ticket_real` (real prod.keys) ok |
| container.rs wiring | tested | `cargo test --release -p napple-build`: all 6 suites ok (container_real: personalized via `Container::load_with_keys`) |
| NCA2 / NCA0 | tested (synthetic only) | `--test nca_versions_real`: 39 NCAs / 11 NSPs, all NCA3, all open |
| bcat storage layout | tested | runtime `build/tests/test_svc_bcat` 137 checks, 0 failed (linked by hand, see below) |
| `cargo test --release -p napple-core` | tested | all suites ok |

## Unbuilt / untested list (the integrator checks these first)
- Nothing is unbuilt. `audio/src/audin_mic.c` non-Apple branch (`#else`) never compiled on Linux/iOS here; it is trivial (stubs).
- `make -C runtime test-services` was NOT run as a whole; I linked only `test_svc_bcat` by hand because `build/tests/test_svc_*` needs `gfx/libnapple_gfx.a`: copied the prebuilt `gfx/libnapple_gfx.a` + `gfx/build/obj/adapter/gfx_napple.o` from the main checkout into this worktree (ignored files) to link the runtime for the boots; a clean `make -C runtime test-services` should be run by the integrator.

## Findings
- libnx `HidNpadInternalState`: gc_trigger_lifo 0x4228, lark_type_l_and_main 0x43E0, struct size 0x43F0 (entry stride 0x5000). The previous 0x4C20 was a recollection error; `runtime` does not read the offset.
- Performance frame: the old format-1 header (0x10, no magic) and the 0x20 format-2 header were wrong; real are 0x18 / 0x30 (+ entry 0x18 in format 2). SMO (REV3), MK8D (REV1), ACNH (REV15) and LAS (REV7) never supply a performance buffer, so no commercial title here exercises this path.
- `DeliveryCacheProgressStatus::Done` = 9 (yuzu), the repo had 6.
- `napple build --update-romfs in-place` refuses an NSZ/NCZ ("compressed container layer"); the message is clear, `materialize` works. Not a bug, worth a line in the Studio help.
- Real `eticket_rsa_keypair` dumps are 0x210 bytes (DEVICE_KEY comment said 0x230).
- `Nca::ctr_sections()` is the only product-code addition (needed by the NCZ writer in tests); sections are returned in file order because section indices need not follow it (SMO's data NCA has index 0 at 0x1C000 and index 1 at 0x148000000-ish).
- Decision (unattended): retro / GC / Palma styles now report HidDeviceTypeBits; the standard styles keep device type 0 (changing them for every title was not asked and could regress titles).
- Outside my area, not fixed: `runtime/src/services/audio.c` comment says the mic is "opt-in until the app carries the usage description"; with this change the default is auto. The generated apps already carry NSMicrophoneUsageDescription.

## How to integrate
- No merge-order constraints. Conflict hotspots: `runtime/src/services/bcat*` and `runtime/tests/test_svc_bcat.c` (nobody else), `audio/src/audren.c` (sched-applets/media-live should not touch it), `input/include/napple_input.h`.
- Build: `make -C audio test`, `make -C input test`, `cargo test --release -p napple-core`, `cargo test --release -p napple-build` (all green here); `make -C runtime test-services` for bcat.
- Linux-only calls: none. Portable C kept for bcat_core (shared with the Linux harness).

## How to test
1. `make -C audio test` (audren 3640, audin_source 52 checks) and `make -C input test` (styles 49).
2. Headless: `NAPPLE_VERBOSE=1 lockf -k /Users/neto/NappleWork/.run.lock tools/boot_test.sh 0100000000010000 60` and `... 0100152000022000 60`, then `grep -i "performance buffer\|audren RequestUpdate" work/logs/<newest>.log`. Pass: no `RequestUpdate failed`, no audren warning; the `audio: performance buffer: N bytes written ...` line appears when the title supplies a buffer. Result on this Mac: all four titles clean (no `RequestUpdate failed`, `audren RequestUpdate error`, or protocol error in the log); no `audio: performance buffer:` line because the titles never ask for performance metrics. Acceptance 2 is therefore only half met (no warning: yes; buffer-written line from a real title: no title does it).
3. NSZ: `export NAPPLE_TESTING_MATERIAL=~/Desktop/switchfiles NAPPLE_NSZ_FULL=1 NAPPLE_NSZ_BLOCK=1 NAPPLE_NSZ_OUT=$TMPDIR/nsz; cargo test --release -p napple-core --test nsz_real -- --nocapture` (5 GB output, 90 s), then `napple build $TMPDIR/nsz/smo.nsz -k <prod.keys> --firmware <zip> --updates none --dlc none --update-romfs materialize --data-only -o <dir>` and compare `manifest.json` module sha256 with the NSP build. Leave NAPPLE_NSZ_OUT unset to skip nothing but write to the temp dir.
4. `cargo test --release -p napple-core --test ticket_real --test nca_versions_real` with the material.
5. Human: none.

## Fix policy hints
Safe to fix directly: compile errors in the non-Apple `audin_mic.c` branch, include order. Design decisions: whether the standard styles should also report device types (currently 0), and whether the audin default should stay auto or become opt-in again (set `NAPPLE_AUDIN_SOURCE=silence` to force off).

## Needs from other areas
- `runtime/src/services/audio.c` (OpenAudioIn comment) and `docs/architecture/runtime-services.md` rows for audren perf / hid GC / bcat should be refreshed to the new facts (not mine to edit).
- `percent-analysis/verified.json`: the seven items can move from partial to impl/verified with the evidence above, except NCA2/NCA0 (stay partial) and the audren boot proof if the boot result above says the title supplied no buffer.
