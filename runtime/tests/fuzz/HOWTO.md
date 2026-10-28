# Fuzzing the C runtime parsers

Drivers for the untrusted-input C parsers live in `runtime/tests/fuzz/`. Findings: `FINDINGS.md`.

## Targets

| name | what it feeds |
|------|---------------|
| `ipc` | HIPC / CMIF / TIPC decoder (`ipc.c`), domain messages, A/B/C/X/W descriptors, and every service that builds on Linux (Linux only) |
| `codec_headers` | H.264 SPS/PPS/avcC/Annex B, VP9, VP8, H.265 headers and the parameter-set writers |
| `jpeg` | the portable JPEG decoder / encoder / scaler |
| `movie_audio` | Vorbis (stb_vorbis + packet wrapper) and OpusHead; needs libopus headers for the Opus path |
| `json` | `src/json.c` (data directory manifest) |
| `amiibo` | amiibo JSON, library documents, base64/hex, NTAG215 dump import, key file, store |
| `libapplet` | error / swkbd / controller / player-select applet storages |
| `romfs_map`, `romfs_overlay` | `romfs.map` and `mods/overlay.json` |
| `nro` | NRO / NRR validation |

Each target file `t_<name>.c` defines `int fuzz_<name>(const uint8_t *, size_t)` and its seed corpus (built from the repo's test vectors).

## Commands (from the repository root)

    make -C Napple/runtime fuzz-build                      # all drivers -> runtime/build/fuzz/<name>_mut
    make -C Napple/runtime fuzz-smoke                      # regression tests + a short deterministic run of every driver (seed 1)
    make -C Napple/runtime fuzz-run FUZZ_SECONDS=300       # 300 s of mutation fuzzing per target, in parallel (-jN); logs in build/fuzz/logs
    make -C Napple/runtime fuzz-run-jpeg FUZZ_SECONDS=300  # one target
    make -C Napple/runtime fuzz-run FUZZ_ENGINE=libfuzzer  # libFuzzer instead (needs clang + libclang-rt-dev; corpora in build/fuzz/corpus-<name>)
    make -C Napple/runtime fuzz-regress                    # the regression tests of every fixed finding (also quick enough for CI)

Needs `clang` with the sanitizer runtimes (Ubuntu: `apt install clang libclang-rt-dev`; `libopus-dev` for the Opus path). `FZ_CC=` picks the compiler.
`FUZZ_SEED`, `FUZZ_SMOKE_ITERS` (default 4000) tune the runs. `fuzz-smoke` takes about 20-30 s in total and is not part of `test` / `test-services`.

## The mutation driver

`build/fuzz/<name>_mut [-n iterations] [-t seconds] [-s seed] [-m max_len] [-o crash_dir] [-q] [-r] [files...]`

xorshift PRNG; bit flips, random bytes, huge 1/2/4/8-byte length fields, small arithmetic, truncation, chunk delete/duplicate, splice from other seeds.
Inputs that reach new code (trace-pc-guard) join the corpus. Built with `-fsanitize=address,undefined -fno-sanitize-recover=all`.
A crash, signal or an input running longer than `FUZZ_TIMEOUT` seconds (20) saves the input and the three before it as
`build/fuzz/crashes/crash-<name>-<hash>[.prevN]` and exits non-zero.

Reproduce: `build/fuzz/<name>_mut -r build/fuzz/crashes/crash-<name>-<hash>` (add the `.prevN` files first in order when the target keeps state, e.g. `ipc`;
`FUZZ_FORK=1` makes `ipc` run each input in its own process).
Then fix, and add `tests/fuzz/regress_<name>.c` (it can `#include "t_<name>.c"` and call `fuzz_<name>` with the embedded input). Regression tests are picked up by the Makefile automatically.

Scratch files go to `/dev/shm/napple-fuzz/<name>` when available (a disk-backed file create costs milliseconds), else `build/fuzz/work`; `FUZZ_WORKDIR` overrides.

## Notes

* The `ipc` target maps a guest-memory arena at 0x20000000 and folds descriptor addresses into it; `runtime_fail` unwinds to the harness (the pthread mutex calls are wrapped to release locks after an unwind).
* The Makefile hook is a single `-include tests/fuzz/fuzz.mk` at the end of `runtime/Makefile`; `make test`, `test-services`, `test-host` are unchanged.
