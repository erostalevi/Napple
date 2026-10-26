# boot-crash (plan6): module-init SIGSEGV did not reproduce on Mac

No source change. The reported `CRASH signal=11 fault=0x38 pc=main+0x111e000` (x0=0, pc==lr) at module init could not be reproduced.

## What was run (Mac, arm64, branch claude/boot-crash-2026-10-24-plan6 at 1570a78)
- Rebuilt gfx + runtime from this checkout (`make -C gfx`, `make -C runtime`).
- `runtime/build/napple-runtime <Napple/work/games/<id>/<hash>> --headless --seconds N` for LAS (01006BB00C6F0000) and SMO (0100000000010000):
  - 45 s, both run concurrently: 0 CRASH lines. LAS: 2304 gfx frames, movie decoder ran (2400 movie frames, ~0.8 ms/frame), audio renderer opened (null sink). SMO: 2206 frames, 183920 draws, audio renderer opened.
  - 3 rounds x 2 games x 2 binaries (this build and ~/Desktop/NappleLATEST/bin, main a0276af) at 10 s: 12/12 runs with 0 CRASH lines.
- `make -C runtime test`: all suites pass (test_media 56 checks, test_timescale 21 checks, 0 failed).
- `cargo test --workspace`: not run, no cargo on PATH in this session (~/.cargo has only a registry dir).

## Findings
- The only Mac-side crash logs found (`~/Library/Logs/DiagnosticReports/napple-runtime-2026-10-24-1447xx..1449xx.ips`) are all **iOS Simulator** runs (CoreSimulator paths): `napple_drawable_presented` unrecognised selector, a Metal texture-view validation abort in `mtl_tex_view` (MTLSimDriver) and `archive_save` -> `_MTLBinaryArchive legacySerializeToURL` abort. None is the SIGSEGV; iOS is out of scope. The Metal binary-archive SIGABRT is that simulator one; on the Mac the log line is `binary archive: 153 of 153 pipelines loaded precompiled`.
- `$TMPDIR/boot.log` named in the brief was overwritten by this session's own run (same TMPDIR), so the original crash log is gone. The old Napple/work/logs crashes (fault=0x30 pc=main+0x775c00, fault=0x58 sdk+0x2a16c4, signal=10 main+0x107aa64) are from Oct 22-23 and different addresses.
- Likely explanations, unverified: the crash was seen with a different game dir (not the two in work/games; main+0x111e000 is beyond the main+0x101fb28 reached by LAS here), a stale rewritten module set, or the iOS path. Needs the exact command/dir from the reporter.

## Status
Not reproduced, nothing fixed. No regression test added (nothing to pin).
