# Fuzzing the untrusted-input parsers

**Goal.** Users load their own game dumps, mod packs and key files, so every parser must answer a malformed file with an error, never with a panic, a hang, an overflow or an allocation of gigabytes. This guide shows how to run the fuzzers, how to add a target and what to do with a finding. Findings so far: [fuzz findings](../reference/fuzz-findings.md).

There are two layers, on purpose:

| Layer | Where | Runs in | What it is for |
|---|---|---|---|
| Seeded-mutation tests | `crates/*/tests/mutation.rs`, one `#[cfg(test)]` test in `napple-core/src/infrastructure/npdm.rs`, `runtime/tests/fuzz/` (C) | `cargo test --workspace`, `make -C runtime test` | Deterministic (fixed seed), a few seconds, catch regressions in CI. |
| Coverage-guided fuzzers | `fuzz/` (Rust, cargo-fuzz + libFuzzer), `runtime/tests/fuzz/` (C, libFuzzer + ASan/UBSan) | on demand, minutes to hours | Find new bugs. |

## Rust: seeded-mutation tests (part of `cargo test`)

```bash
cargo test -p napple-core --test mutation        # NSP/PFS0, HFS0, XCI, NCA (header + body), NCZ/NSZ/XCZ, NSO, romfs, IVFC, CNMT, NACP, ticket, key file
cargo test -p napple-build --test mutation       # BKTR update NCAs and bucket trees, container load, firmware zip, NRO, NTB1, BFTTF
cargo test -p napple-mods --test mutation        # IPS, pchtxt (+JSON), cheats, manifest, config, .napmod zip
cargo test -p napple-rewrite --test mutation     # NSO layout, MOD0, dynamic section, RELR/RELA/JMPREL, eh_frame_hdr, rewrite_module
```

Each test mutates synthetic seeds (the public fixtures in `napple-core/tests/fixtures/nsz`, `docs/mods/examples`, small builders) with bit flips, byte replacement, truncation, huge 32/64-bit length fields (`0xffffffff`, `0x7fffffffffffffff`, ...), zeroed ranges, splices and zero extension. NCA headers are encrypted, so the NCA tests also decrypt the header with the synthetic key, mutate the structural fields and encrypt it again (`tests/mutnca`). A custom global allocator records the largest single allocation per case: a tiny input that makes a parser reserve more than 128 MiB fails the test even if the allocation would succeed, and a case slower than 20 s fails too (hang guard).

Knobs (environment): `NAPPLE_MUT_ITERS` (default 200 per target), `NAPPLE_MUT_SEED`. A longer soak: `NAPPLE_MUT_ITERS=30000 NAPPLE_MUT_SEED=7 cargo test -p napple-rewrite --test mutation`. The helpers live in `crates/napple-core/tests/mutlib` (mutator, runner, allocator guard) and are included by the other crates with `#[path]`.

## Rust: cargo-fuzz targets (`Napple/fuzz`)

The `fuzz/` crate has its own `[workspace]` table, so `cargo build/test --workspace` never touches it.

```bash
rustup toolchain install nightly --profile minimal
cargo install cargo-fuzz
cd Napple/fuzz
cargo run --release --bin mkcorpus            # seed corpus into fuzz/corpus/<target>/ (synthetic fixtures only, git-ignored)
cargo +nightly fuzz list                      # the targets
cargo +nightly fuzz run nca -- -max_total_time=300 -timeout=20 -rss_limit_mb=2048
./run_all.sh 300 3                            # every target, 300 s each, 3 in parallel; logs in fuzz/logs, exit 1 on any crash
```

Targets: `pfs0 hfs0 xci nca ncz nso nso_rewrite romfs ivfc cnmt nacp ticket npdm keyfile ingest bktr_nca bktr_tables zip_firmware nro ips pchtxt cheats manifest_config napmod`.

* **NCA targets take the DECRYPTED header.** `nca`, `ncz` and `bktr_nca` encrypt the first 0xC00 bytes with the synthetic header key before parsing, otherwise the fuzzer never gets past the XTS layer. The seed corpus is stored in that form. Use `cargo run --release --bin mkcorpus -- encrypt <crash-file> <out>` to turn a crash input into the real (encrypted) file.
* cargo-fuzz builds with debug assertions, so arithmetic overflow is a crash (as in `cargo test`; release builds only wrap, which is a wrong result instead of a panic).
* Crashes land in `fuzz/artifacts/<target>/`. Reproduce with `cargo +nightly fuzz run <target> fuzz/artifacts/<target>/crash-...`.

### Fixing a finding

1. Reproduce, minimise (`cargo +nightly fuzz tmin <target> <crash>`).
2. Fix the root cause: a bounds check, `checked_add`/`saturating_add`, or an error return instead of an unwrap or an up-front `Vec::with_capacity(untrusted)`. Valid input must behave exactly as before.
3. Add a regression test: a hand-built input in the crate's `mutation.rs` (or a unit test next to the parser), or the minimised crash, re-encrypted for NCAs, saved under `crates/<crate>/tests/regress/`. Check that it fails without the fix.
4. Add a row to [fuzz findings](../reference/fuzz-findings.md).

### Adding a target

Add `fuzz/fuzz_targets/<name>.rs` (copy a small one, e.g. `cnmt.rs`), a `[[bin]]` entry in `fuzz/Cargo.toml`, seeds in `mkcorpus.rs`, and a case in the matching `mutation.rs`. Touch what the parser hands back (read the files, open the sections) so lazy readers are exercised too.

## C runtime parsers (Linux harness)

Drivers: `runtime/tests/fuzz/` (also documented in `runtime/tests/fuzz/HOWTO.md`). Linux only; needs `clang`, `libclang-rt-dev` and `libopus-dev`.

Drivers for the untrusted-input C parsers live in `runtime/tests/fuzz/`. Findings: [fuzz findings](../reference/fuzz-findings.md).

### Targets

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

### Commands (from the repository root)

    make -C Napple/runtime fuzz-build                      # all drivers -> runtime/build/fuzz/<name>_mut
    make -C Napple/runtime fuzz-smoke                      # regression tests + a short deterministic run of every driver (seed 1)
    make -C Napple/runtime fuzz-run FUZZ_SECONDS=300       # 300 s of mutation fuzzing per target, in parallel (-jN); logs in build/fuzz/logs
    make -C Napple/runtime fuzz-run-jpeg FUZZ_SECONDS=300  # one target
    make -C Napple/runtime fuzz-run FUZZ_ENGINE=libfuzzer  # libFuzzer instead (needs clang + libclang-rt-dev; corpora in build/fuzz/corpus-<name>)
    make -C Napple/runtime fuzz-regress                    # the regression tests of every fixed finding (also quick enough for CI)

Needs `clang` with the sanitizer runtimes (Ubuntu: `apt install clang libclang-rt-dev`; `libopus-dev` for the Opus path). `FZ_CC=` picks the compiler.
`FUZZ_SEED`, `FUZZ_SMOKE_ITERS` (default 4000) tune the runs. `fuzz-smoke` takes about 20-30 s in total and is not part of `test` / `test-services`.

### The mutation driver

`build/fuzz/<name>_mut [-n iterations] [-t seconds] [-s seed] [-m max_len] [-o crash_dir] [-q] [-r] [files...]`

xorshift PRNG; bit flips, random bytes, huge 1/2/4/8-byte length fields, small arithmetic, truncation, chunk delete/duplicate, splice from other seeds.
Inputs that reach new code (trace-pc-guard) join the corpus. Built with `-fsanitize=address,undefined -fno-sanitize-recover=all`.
A crash, signal or an input running longer than `FUZZ_TIMEOUT` seconds (20) saves the input and the three before it as
`build/fuzz/crashes/crash-<name>-<hash>[.prevN]` and exits non-zero.

Reproduce: `build/fuzz/<name>_mut -r build/fuzz/crashes/crash-<name>-<hash>` (add the `.prevN` files first in order when the target keeps state, e.g. `ipc`;
`FUZZ_FORK=1` makes `ipc` run each input in its own process).
Then fix, and add `tests/fuzz/regress_<name>.c` (it can `#include "t_<name>.c"` and call `fuzz_<name>` with the embedded input). Regression tests are picked up by the Makefile automatically.

Scratch files go to `/dev/shm/napple-fuzz/<name>` when available (a disk-backed file create costs milliseconds), else `build/fuzz/work`; `FUZZ_WORKDIR` overrides.

### Notes

* The `ipc` target maps a guest-memory arena at 0x20000000 and folds descriptor addresses into it; `runtime_fail` unwinds to the harness (the pthread mutex calls are wrapped to release locks after an unwind).
* The Makefile hook is a single `-include tests/fuzz/fuzz.mk` at the end of `runtime/Makefile`; `make test`, `test-services`, `test-host` are unchanged.
