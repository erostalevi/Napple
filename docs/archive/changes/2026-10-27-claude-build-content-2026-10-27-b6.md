# build-content (b6): HtmlDocument extraction, NSZ/XCZ hardening tests

- NSZ/XCZ: NCZ (solid + NCZBLOCK) reading already existed in napple-core (`infrastructure/ncz.rs`, wired in `application/ingest.rs`). Added `truncated_and_corrupt_nsz_do_not_panic` (cuts and byte flips over stream and block NCZ inside an NSZ). No reader change was needed. *tested*.
- HtmlDocument: new `crates/napple-build/src/html.rs`; `pipeline.rs` reads the newest HtmlDocument NCA and writes `html/` into every version dir. Layout documented in `docs/architecture/module-format.md`. E2E `html_document_written_next_to_romfs_map` (synthetic NCA, also checks no `html/` without one). *tested*.
- `cargo test --workspace` green on Linux. Not verified on macOS / with a real title.
