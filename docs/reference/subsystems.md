# Subsystem reference

One entry per subsystem: what it does, where the code is, how to test it, which env vars show it working, and the symptoms that point at it
([symptoms.md](../symptoms.md)). Design documents: [overview](../architecture/overview.md), [graphics](../architecture/gfx.md), [runtime services](../architecture/runtime-services.md),
[data directory and module tables](../architecture/module-format.md), [iOS](../architecture/ios.md).

## Runtime core (loader, kernel, threads)
- Code: `runtime/src/main.c` (boot), `module.c`, `nro.c`, `ro.c`, `npdm.c` (loaders), `kernel.c`, `kcode.c`, `ksession.c`, `kmsg.c` (svc and kernel objects), `thread.c`, `threads.c`, `memory.c`, `signals.c` (crash handler), `gate.S`, `thread_enter.S`.
- Test: `make -C runtime test` (host tests), `make -C runtime test-host` (portable kernel and loader code on any POSIX host). Debug: `NAPPLE_PCPROF=1` (PC profile per thread), `NAPPLE_TRACE_IPC=all`, `NAPPLE_SVC_RING=1`, `NAPPLE_THREAD_LOG=1`, `kill -USR1 <pid>` (thread dump).
- Symptoms: boot spins on `WaitSynchronization(handle 0)`, `CRASH signal=11`.

## IPC and HLE services
- Code: `runtime/src/ipc.c` (CMIF and TIPC framing, `sm`), `dispatch.c`, `runtime/src/services/*.c` (one file per service, `*_core.c` holds the portable logic that unit tests link). Table: `services/services.c`.
- Test: `make -C runtime test-services` (Linux-portable core logic also in `tools/linux-check/run-tests.sh`); `make -C runtime test-ipc-reference` checks every app-reachable command id against `runtime/tests/ipc_reference/`.
- Debug: `NAPPLE_TRACE_IPC=all` (the only value the code recognises), `NAPPLE_IPC_LENIENT=1` (list several missing commands in one run).
- Symptoms: abort "request on handle 0", wrong display version, `time:u` errors. Reference tables: [runtime-services.md](../architecture/runtime-services.md).

## Filesystem, RomFS, saves
- Code: `runtime/src/services/fs.c` (storages, save mounts, save journal), `services/romfs_map.c` (in-place maps, the mod RomFS overlay), `services/romfs_read.c` (RomFS as an IFileSystem), `crates/napple-build` (RomFS building, `--update-romfs`).
- Test: `make -C runtime test` (`test_fs_journal`, `test_fs_savedata`, `test_svc_fs`). Debug: `NAPPLE_FS_TRACE=1`.
- Notes: `napple build ... --data-only --update-romfs in-place` for small test data ([building test data](../titles/README.md#building-test-data-without-disk-pain-all-titles)).

## Graphics (NVN to Metal)
- Code: `gfx/src/api_*.c` (NVN API surface, command recording: `api_cmd.c`), `mtl_replay.m` (command replay to Metal), `mtl_present.m` (presentation, perf log), `mtl_tex.m` + `tex/` (textures, layouts, deswizzle), `mtl_shader.m` + `shader/maxwell_translate.c` (Maxwell ISA to MSL, pipelines, caches), `mtl_scale.m` (render scale), `dirty.c` (guest write tracking).
- Test: `make -C gfx test` (headless Metal probes, `MTL_DEBUG_LAYER=1`). Shader corpus runs (`NAPPLE_SHADER_CORPUS`) need programs extracted from game data ([GPU title scan](../guides/gpu-title-scan.md)). Design and interface: [graphics](../architecture/gfx.md).
- Debug: `NAPPLE_GFX_CMDTRACE=1`, `NAPPLE_PERF_LOG=1`, `NAPPLE_GFX_PROF=1`, `NAPPLE_SCREENSHOT`, `NAPPLE_SHADER_DUMP`, `NAPPLE_DRAWLOG_RT_ALL=1`, `NAPPLE_SCALE_DIAG=1`, `NAPPLE_DEBUG_SKIP_VS` (bisect one vertex program per frame), `NAPPLE_GFX_MATH`, `NAPPLE_REC_BUDGET_MB`, `NAPPLE_NO_ORPHAN=1`.
- Symptoms: black image, exploded geometry, stale command handles, first-use pipeline stutter, a frame-rate ceiling set by GPU sync waits.

## Audio
- Code: `audio/src/` (`audren.c` renderer, `mix.c`, `effects.c`, `reverb.c`, `audout.c`, `audin.c`, `audin_source.c`, `audin_mic.c`, `hwopus.c`, `engine.c` CoreAudio, `engine_ios.m`). Test: `make -C audio test`. Debug: `NAPPLE_AUDIO_STATS=1`, `NAPPLE_AUDIO_DEVICE=1` (headless output).

## Input and HID
- Code: `input/src/hid_shm.c` (HID shared memory writer: lifo rings, outer sampling number = 2n), `gamepad.m`, `bindings.c`, `motion.c`. Test: `make -C input test`. Scripted input for headless runs: `NAPPLE_INPUT_SCRIPT="15:A,19:A:0.3"`.
- Symptoms: game boots but never draws (Npad reader retry loop).

## Frontend (window, settings, overlay)
- Code: `frontend/src/` (`frontend.m`, `overlay.m`, `settings.c`, `textbar.m`, `ios/`). Test: `make -C frontend test`.

## AArch32 (NTB2) titles
- Code: `runtime/src/a32/` (module loader, svc adapter, NVN marshalling, out-of-line operations), `runtime/src/a32jit/` (lifter `a32lift.c`, dispatcher `a32jit.c`), `crates/napple-build/src/tables32.rs`.
- Test: `make -C runtime test-a32`, `python3 runtime/tests/a32/fuzz_all.py`. Debug: `NAPPLE_A32_JITSTATS=1`, `NAPPLE_A32_SVCTRACE=1`. See [aarch32](../architecture/aarch32.md).

## Build pipeline (napple CLI)
- Code: `crates/napple-core` (containers, NCA, BKTR, keys), `napple-build` (pipeline, iOS packaging), `napple-rewrite` (A64 rewriter), `napple-sigs`, `napple-compat`, `napple-mods`, `napple-cli`. Test: `cargo test --workspace`. Docs: [rewrite-format.md](../architecture/rewrite-format.md), [compat-format.md](../architecture/compat-format.md), [CLI](cli.md).

## Napple Studio and packaging
- Code: `studio/Sources` (SwiftUI app, `NappleStudioCore` has the testable logic), `packaging/` (launcher), `studio/build_app.sh`. Test: `swift test --package-path studio`. Protocol: [studio-protocol.md](../architecture/studio-protocol.md).

## iOS
- Code: `tools/build_ios.sh`, `tools/build_ios_deps.sh` (libopus, libvpx), `crates/napple-build/src/ios.rs`, `runtime/src/ios_*.{c,m}`, `frontend/src/ios`. Doc: [ios.md](../architecture/ios.md). Simulator smoke (compile only): `SDK=iphonesimulator tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh`.

## Media (movies, host1x engines, album)
- Code: `runtime/src/movie.c` (host movie decoders), `vt_h264.c`, `services/decoder_vt.c`, `services/nvdrv_media_core.c` (NVDEC, VIC, NVJPG, MSENC), `services/jpeg_core.c`, `services/caps*.c`, `services/movie_audio_core.c`.
- Test: `make -C runtime test` (`test_svc_nvdrv_media`, `test_svc_movie_audio`, `test_svc_caps_e2e`). Debug: `NAPPLE_MEDIA_TRACE=1`, `NAPPLE_VIC_DUMP`, `NAPPLE_NVDEC_DUMP`.

## Applets
- Code: `runtime/src/services/applet.c`, `applet_ae.c`, `applet_lib_core.c`, `applet_web_core.c`, `applet_album_core.c`; sheets in `frontend/src/applet_sheets.m`.
- Test: `runtime/tests/linux/run_applet.sh`, `make -C runtime test`. Scripted answers: `NAPPLE_SWKBD_ANSWER`, `NAPPLE_PLAYER_SELECT`, `NAPPLE_CONTROLLER_SUPPORT`, `NAPPLE_WEB_ANSWER`, `NAPPLE_ALBUM_PICK`.

## Network and Napple Online
- Code: `runtime/src/services/online_core.c` (provider interface), `runtime/src/net/` (host provider, rooms, online client glue), `crates/napple-online`, `napple-account`, `napple-social`, `napple-room`, `napple-cloud`.
- Test: `cargo test --workspace`, `make -C runtime test`, the end-to-end run in [online-e2e](../guides/online-e2e.md). Debug: `NAPPLE_NET_LOG=1`. Guides: [network providers](../guides/network-providers.md), [Napple Online](../guides/napple-online.md).

## Mods
- Code: `crates/napple-mods` (format, apply, convert), `runtime/src/mods_runtime.c`, `mods_cheats.c` (cheat VM), `services/romfs_map.c` (RomFS overlay). Test: `cargo test -p napple-mods`. Off switch: `NAPPLE_NO_MODS=1`. Docs: [mods](../mods/README.md).

## CI and docs checks
- `.github/workflows/tests.yml` (the same commands as `tools/ci_build.sh`), `tools/check_docs.py` (links, anchors, orphans, symptoms table, generated support tables).
