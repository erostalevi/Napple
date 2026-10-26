## What and why

<!-- What does this change, and why? Link the issue or change note. -->

## How it was tested

<!-- Commands you ran and results. Use honest status words: verified / tested / unbuilt / untested / proposed. -->

- [ ] `cargo test --workspace` / `cargo clippy --workspace --all-targets`
- [ ] `make -C runtime test test-services test-host`
- [ ] `make -C gfx test` / `swift test` (macOS only)
- [ ] `python3 tools/check_docs.py`

## Not tested / known gaps

<!-- Anything that could not run on your machine, and anything left open. -->

## Checklist

- [ ] Focused change; formatting follows `.clang-format` / `rustfmt.toml` for the files I touched
- [ ] Docs updated (and a note in `docs/changes/` for user-visible or architectural changes)
- [ ] No game files, keys, firmware or data derived from them are included
