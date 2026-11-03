# Fuzz hardening of the untrusted-input parsers (2026-10-27)

Branch `claude/fuzz-hardening-2026-10-27`, base `11a9da9`. Cloud (Linux): **tested, not verified**; the macOS-only runtime targets (`make -C runtime test test-services`) were not run here.

## What changed
* Seeded-mutation tests that run in `cargo test`: `napple-core/tests/mutation.rs` (PFS0/HFS0/XCI, NCA header+body, NCZ/NSZ/XCZ, NSO, romfs, IVFC, CNMT, NACP, ticket, key file, ingest), `napple-build/tests/mutation.rs` (BKTR, bucket trees, Container, firmware zip, NRO, NTB1, BFTTF), `napple-mods/tests/mutation.rs` (IPS, pchtxt, cheats, manifest, config, `.napmod`), `napple-rewrite/tests/mutation.rs` (module layout, MOD0, dynamic section, RELR/RELA, rewriter), and one in `napple-core/src/infrastructure/npdm.rs`. Shared helpers: `napple-core/tests/mutlib` (mutator, allocation guard: >128 MiB single allocation or >20 s fails) and `tests/mutnca` (decrypt header, mutate, re-encrypt).
* `Napple/fuzz/`: cargo-fuzz crate with its own `[workspace]` (24 targets, `mkcorpus`, `run_all.sh`), so `cargo test --workspace` never builds it.
* `runtime/tests/fuzz/`: C drivers (10 targets, libFuzzer entry + seeded mutation driver, ASan+UBSan), `fuzz.mk` hooked by one appended line in `runtime/Makefile` (`fuzz-build`, `fuzz-smoke`, `fuzz-run`, `fuzz-regress`).
* Fixes: 7 Rust findings and 19 C findings, every one with a regression test. Table: [fuzz findings](../../reference/fuzz-findings.md). Guide: [fuzzing](../../guides/fuzzing.md).

## Parser files touched (minimal diffs, no behaviour change on valid input)
Rust: `napple-core` `nso.rs`, `ncz.rs`, `nca.rs`, `domain/keys.rs`; `napple-build` `zipread.rs`, `bktr.rs`; `napple-rewrite` `relocs.rs`, `image.rs`. C: `runtime/src/ipc.c`, `ipc.h`, `json.c`, `services/{jpeg_core,movie_audio_core,amiibo_core,amiibo_crypto_core,codec_headers_core,romfs_map,offline,time,time_core}.c`, `third_party/stb_vorbis.c`.

## Verified vs not
* tested (Linux): `cargo test --workspace` green; `cargo clippy --workspace --all-targets -- -D warnings` green except the pre-existing unknown lint `clippy::chunks_exact_to_as_chunks` in `nca.rs` (newer clippy than this toolchain; run with `-A unknown-lints`); `cargo fmt --check` clean; `runtime/tests/linux/run.sh` and `run_applet.sh`; `make -C runtime fuzz-regress`.
* fuzz budget: Rust 24 targets x 301 s (about 183M executions) and C 10 targets x 300 s, zero open crashes after the last fix.
* not run: `make -C runtime test test-services` (needs macOS headers; `hle_stack.c` includes `<mach/mach.h>`, fails identically before the change), libFuzzer builds of the C targets. **Remaining Mac check:** run `make -C runtime test test-services test-host` after merging the C parser fixes.
* The `ipc` fixes are a start (target reached about 2000 edges); `hid_core`, audio decoders and most `fs.c` handlers were not swept.
* CI needs `apt install clang libclang-rt-dev libopus-dev` for the C fuzz drivers.
