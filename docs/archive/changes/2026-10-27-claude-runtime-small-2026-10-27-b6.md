# 2026-10-27 runtime-small (b6): vi producer, npad style helpers, unused-core tests

Branch claude/runtime-small-2026-10-27-b6 (Linux cloud; assignment orchestrations/runtime-small.md). Base 34ce68b. Status words: tested on Linux, not verified on macOS.

## Result
- **vi `TransactParcel` / `TransactParcelAuto`** (vi.c, vi_core.[ch]): rows are now `IPC_IMPL`. `vi_producer_transact` keeps a per-binder IGraphicBufferProducer state (connected, 3 slots dequeued /
  free, window size and format, frame counter): connect returns QueueBufferOutput with the window size, query answers width / height / format / min undequeued, dequeue hands out slots 0..2 and
  answers busy when all are out, request / queue / cancel need a dequeued slot (else -EINVAL), anything before connect answers -ENODEV, disconnect clears, an unknown code answers -EINVAL.
  The parcel (header, strict-mode word, interface token, arguments) is parsed in vi.c. No pixels move (the graphics back end still bypasses the producer). Codes and layouts from memory: unverified.
- **npad styles** (hid_core.[ch], hid.c): `HIDC_STYLE_*` constants (libnx bits incl. GameCube 5, Palma 6, Lark/NES 7, Lucia, Lagon, Lager), `hidc_styles_sanitize`, `hidc_style_name`,
  `hidc_style_supported`, `hidc_style_is_retro`. `SetSupportedNpadStyleSet` passes only owned bits to the input backend and logs GameCube / Palma / NES support; `ApplyNpadSystemCommonPolicy`
  now supports every pad style (0xfff: it said "every style" but only set 0x1f). hid.c is macOS-only: **unbuilt**.
- **Unused core API**: `test_svc_unused_core.c` covers mii_db_set_storage (in-memory provider), mii_new_create_id, time_info_read, bsd_lookup, irs_core_access_done, nfp_build_{tag,common,model,register}_info,
  ngc_words_clear, amiibo_format (28 checks).

## Found already done / not changed
- **audin**: the registered `audin:u` is the one in audio.c over the audio library, which already has the capture source hook (`napple_audin_set_source`, `NAPPLE_AUDIN_SOURCE`); `audin.c` + `audin_core`
  are only the unit-test copy. A duplicate hook added to audin_core was reverted (9f903a2 + its revert); nothing to add for the CoreAudio glue on the runtime side.
- **audren performance metrics**: the renderer is in `audio/src/audren.c` (done by small-catalog: node ids, detail entries); audio.c only passes the performance buffer through. Not owned here; the header sizes
  and the timing model remain the open (memory-limited) items listed in 2026-10-27-claude-small-catalog.

## Tests (Linux harness, runtime/tests/linux/run.sh)
test_svc_vi 56 -> 97, test_svc_hidcore 31 -> 40, test_svc_unused_core new (28), known calls 705 green. Full run: 47 built and ran; `svc_mii` check 35 fails identically on the base commit
(the container user has no name the test expects), the applet / audio / fs / time tests do not build on Linux.
Mac checks to run: `make -C runtime test test-services test-host`, LAS / SMO / ACNH / MK8D 45 s headless boots (audio off).
