# audio-small (batch 2026-10-24-plan5): audrec:u, svc 0x23, shader IMAD and LEA; I3DL2 already present

Linux cloud session. Nothing here was compiled or run on macOS.

## I3DL2 reverb (audren): no change needed
The effect already exists: `audio/src/reverb.{c,h}` is the I3DL2 reverb (effect type 5, `Reverb3dParam`), wired into `mix.c` (`mixer_update_effects`, the
`e->type==5` branch of the effect loop) and covered by `audio/tests/test_reverb3d.c` (impulse response, RT60, dry path, linearity, stability, validation; see
`2026-10-24-audio-i3dl2-audin-review.md`). It landed in the base commit before this batch's coverage numbers were taken, so the +0.39 is already earned.
I changed no audio code. `test_reverb3d` was re-run here (54 checks, 0 failed), built without `hwopus.c` because libopus headers are missing on this host.
The other effect types (1 mix, 2 aux, 3 delay, 4 reverb, 6 biquad, 7 limiter, 8 capture, 9 compressor) are all handled in `mix.c` too.

## audrec:u
`runtime/src/services/audrec.c`, `audrec_core.[ch]`, registered in `services.c`, test `runtime/tests/test_svc_audrec.c`. `OpenFinalOutputRecorder` (cmd 0)
returns {48000, 2, PCM16, Stopped} and an `IFinalOutputRecorder` (cmds 0-9: state, start, stop, append, buffer event, released buffers, contains, end time,
and the Auto variants). The host has no tap on the mix, so a started recorder records silence; appended buffers are zero-filled and complete at once.
Ids and layouts are from memory (unverified), like `audin.c`. Result code on a full queue / unknown buffer reuses audio module 153 description 8 (a guess).

## svc 0x23 SendAsyncRequestWithUserBuffer
`runtime/src/kernel.c`: delivers like 0x22 (through the registered 0x22 handler, args remapped x1/x2/w3 -> x0/x1/w2) and returns an already-signalled readable event
in w1 (the writable end is closed). A request to a guest server thread blocks until the server replies (no background delivery). Host test:
`runtime/tests/host/test_svc_async.c` (`make -C runtime test-host`).

## Maxwell IMAD and LEA
`gfx/src/shader/maxwell_translate.c`, tests in `gfx/tests/t_xlate.m`. Layouts are from memory and no word in the LAS or SMO corpora uses them; the layouts are
documented in comments at the decoder. IMAD: reg 0x5a, cbuf 0x4a, imm 0x34, and the 0x52 (B = REG_39, C = cbuf) form; bit 48 signed, 49 negate c, 51 negate
product, 53 .HI. IMAD.CC / .X are refused cleanly. LEA.LO (reg 0x5bd0, cbuf 0x4bd0, imm 0x36d0; scale [39,44), bit 45 negate B, .CC 47, .X 48) and LEA.HI
(0x5bd8, 0x4bd8; scale [28,33), c = REG_39, .X 48). No other op was touched. `TRANSLATOR_VERSION` is NOT bumped: these opcodes failed as "unknown instruction"
before, so no cached MSL changes (it still reads 6u).

## Verified vs not
tested on Linux: `test_svc_audrec` (61 checks), `test_svc_async` (7), `make test-host` (all), `t_xlate_linux.sh` (ok, Metal compile checks skipped), `test_reverb3d`.
Not run: any macOS build, Metal compile of the new MSL, a real guest using audrec:u / svc 0x23 / IMAD / LEA.
