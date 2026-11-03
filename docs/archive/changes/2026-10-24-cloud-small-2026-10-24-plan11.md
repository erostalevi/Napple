# cloud-small: ncm, caps and audren performance metrics (2026-10-24)

- **ncm** (`runtime/src/services/ncm.c`): the content-meta database and content storage are real in-memory tables instead of "always empty". Set/Get/Remove/Has/HasAll/List/ListApplication/GetLatestContentMetaKey/GetSize/GetRequiredSystemVersion/GetPatchId/GetAttributes and placeholders/Register/Has/Delete/ListPlaceHolder/GetContentCount/ListContentId/GetSizeFromContentId. Starts empty (the runtime knows no installed title set), nothing persisted. Scan: 0/47 -> 40/7 impl/stub rows. Command layouts are unverified.
- **caps** (`caps.c`, `caps_core.{c,h}`): an in-memory album (64 entries). Saves add entries, lists return them, size/load/thumbnail of an entry yield 0 bytes, delete removes, unknown entries are CaptureNotFound. Scan: 0/27 -> 27/0.
- **audren** (`audio/src/audren.c`): `write_performance` fills the performance buffer (header + modelled per-voice/mix/sink entries) and the byte count in the update output. Layout unverified.
- `audio/tests/linux/include/TargetConditionals.h`: stand-in so `make -f Makefile.linux` builds engine.c.

Status: **tested** on Linux only (runtime host harness for ncm/caps, `make -f Makefile.linux test` for audio). Not built or run on macOS.
