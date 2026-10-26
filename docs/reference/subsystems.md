# Subsystem reference

One entry per subsystem: what it does, where the code is, how to test it, which env vars show it working, and the symptoms that point at it
([symptoms.md](../symptoms.md)). Design documents: [ARCHITECTURE.md](../architecture/overview.md), [gfx-design.md](../architecture/gfx.md), [runtime-services.md](../architecture/runtime-services.md),
[runtime-modules.md](../architecture/module-format.md), [ios.md](../architecture/ios.md). Per-title state: [titles/](../titles/README.md).

## Runtime core (loader, kernel, threads)
- Code: `runtime/src/main.c` (boot), `module.c`, `nro.c`, `ro.c`, `npdm.c` (loaders), `kernel.c`, `kcode.c`, `ksession.c`, `kmsg.c` (svc and kernel objects), `thread.c`, `threads.c`, `memory.c`, `signals.c` (crash handler), `gate.S`, `thread_enter.S`.
- Test: `make -C runtime test` (host tests). Debug: `NAPPLE_PCPROF=1` (PC profile per thread), `NAPPLE_TRACE_IPC=all`.
- Symptoms: boot spins on `WaitSynchronization(handle 0)`, `CRASH signal=11`.

## IPC and HLE services
- Code: `runtime/src/ipc.c` (CMIF and TIPC framing, `sm`), `dispatch.c`, `runtime/src/services/*.c` (one file per service, `*_core.c` holds the portable logic that unit tests link). Table: `services/services.c`.
- Test: `make -C runtime test-services` (Linux-portable core logic also in `tools/linux-check/run-tests.sh`).
- Debug: `NAPPLE_TRACE_IPC=all` (the only value the code recognises).
- Symptoms: abort "request on handle 0", wrong display version, `time:u` errors. Reference tables: [runtime-services.md](../architecture/runtime-services.md).

## Filesystem, RomFS, saves
- Code: `runtime/src/services/fs.c` (storages, save mounts), `services/romfs_map.c` (in-place maps, on the mk8d-64 branch), `crates/napple-build` (RomFS building, `--update-romfs`).
- Notes: `napple build ... --data-only --update-romfs in-place` for small test data (see [titles/](../titles/README.md)).

## Graphics (NVN to Metal)
- Code: `gfx/src/api_*.c` (NVN API surface, command recording: `api_cmd.c`), `mtl_replay.m` (command replay to Metal), `mtl_present.m` (presentation, perf log), `mtl_tex.m` + `tex/` (textures, layouts, deswizzle), `mtl_shader.m` + `shader/maxwell_translate.c` (Maxwell ISA to MSL, pipelines, caches), `mtl_scale.m` (render scale), `dirty.c` (guest write tracking).
- Test: `make -C gfx test` (headless Metal probes, `MTL_DEBUG_LAYER=1`). Corpus guards `t_las`, `t_smo` need game data. Design: [gfx-design.md](../architecture/gfx.md), [gfx-interface.md](../architecture/gfx.md).
- Debug: `NAPPLE_GFX_CMDTRACE=1`, `NAPPLE_PERF_LOG=1`, `NAPPLE_GFX_PROF=1`, `NAPPLE_SCREENSHOT`, `NAPPLE_SHADER_DUMP`, `NAPPLE_DRAWLOG_RT_ALL=1`, `NAPPLE_SCALE_DIAG=1`, `NAPPLE_DEBUG_SKIP_VS` (bisect one vertex program per frame), `NAPPLE_GFX_MATH`, `NAPPLE_REC_BUDGET_MB`, `NAPPLE_NO_ORPHAN=1`.
- Symptoms: black image, exploded geometry, stale command handles, stutter, 45 fps ceiling.

## Audio
- Code: `audio/src/` (`audren.c` renderer, `mix.c`, `effects.c`, `reverb.c`, `audout.c`, `audin.c`, `hwopus.c`, `engine.c` CoreAudio, `engine_ios.m`). Test: `make -C audio test`.

## Input and HID
- Code: `input/src/hid_shm.c` (HID shared memory writer: lifo rings, outer sampling number = 2n), `gamepad.m`, `bindings.c`, `motion.c`. Test: `make -C input test`. Scripted input for headless runs: `NAPPLE_INPUT_SCRIPT="15:A,19:A:0.3"`.
- Symptoms: game boots but never draws (Npad reader retry loop).

## Frontend (window, settings, overlay)
- Code: `frontend/src/` (`frontend.m`, `overlay.m`, `settings.c`, `textbar.m`, `ios/`). Test: `make -C frontend test`.

## AArch32 (NTB2) titles
- Code: `runtime/src/a32/`, plus the lifted `a32jit.dylib` build (its tools script is on the mk8d branches). `NAPPLE_A32_JIT=0` selects the interpreter. See [rewrite-format.md](../architecture/rewrite-format.md) for the AOT format.

## Build pipeline (napple CLI)
- Code: `crates/napple-core` (containers, NCA, BKTR, keys), `napple-build` (pipeline, iOS packaging), `napple-rewrite` (A64 rewriter), `napple-sigs`, `napple-compat`, `napple-cli`. Test: `cargo test --workspace`. Docs: [rewrite-format.md](../architecture/rewrite-format.md), [compat-format.md](../architecture/compat-format.md).

## Napple Studio and packaging
- Code: `studio/Sources` (SwiftUI app, `NappleStudioCore` has the testable logic), `packaging/` (launcher), `studio/build_app.sh`. Test: `swift test --package-path studio`. Protocol: [studio-protocol.md](../architecture/studio-protocol.md).

## iOS
- Code: `tools/build_ios.sh`, `tools/build_ios_deps.sh` (libopus, libvpx), `crates/napple-build/src/ios.rs`, `runtime/src/ios_*.{c,m}`, `frontend/src/ios`. Doc: [ios.md](../architecture/ios.md). Simulator smoke (compile only): `SDK=iphonesimulator tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh`.

## CI and docs checks
- `.github/workflows/tests.yml` (the same commands as `tools/ci_build.sh` and `napple-latest`), `tools/check_docs.py` (links and anchors).
