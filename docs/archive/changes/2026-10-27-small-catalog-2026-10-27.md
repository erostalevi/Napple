# 2026-10-27 small-catalog: audren performance, npad styles, IR tera / pointing processors, bcat layouts checked and tested

Branch claude/small-catalog-2026-10-27 (Linux cloud session; assignment orchestrations/small-catalog.md). Base 06a0205 (orchestrate/2026-10-27-b3).

## Honest note on the references
The session had no network access (switchbrew, Ryujinx and yuzu could not be fetched), so every "checked against" below means checked against the author's memory of
those references, written into the comments as such. Where memory was not good enough to change a layout, it was left alone and listed under "Not changed". Nothing is verified on a console or a title.

## Result
- **IR camera (irs.c, irs_core.[ch])**: RunTeraPluginProcessor (310) and RunDpdProcessor (312) are implemented over the frame source: the tera processor delivers a 20x15 box-downsampled frame
  (0x12c bytes, the size the public description gives) and the pointing processor delivers up to four markers (brightest clusters, positions normalised to -1..1) and the pointer (midpoint of the two
  brightest). Config sizes (4 and 32 bytes) follow the public description; state field order is this simulator's choice, labelled unverified. Scan stub rows in irs.c: **2 -> 0** (impl 16 -> 18).
- **npad styles (napple_input.h, hid_shm.c, input.c)**: the style tag bits disagreed with libnx / yuzu / Ryujinx (Lark HVC / NES left / right had a bit each at 7..14, Lucia / Lagon / Lager sat at 15..17).
  Now: Lark 7, HandheldLark 8, Lucia 9, Lagon 10, Lager 11 (SystemExt 29, System 30 unchanged). The old names stay as aliases of the new bits. The GameCube trigger lifo (NpadGcTriggerState
  {sampling, l, r} at entry offset 0x4C20, 0x1B8 bytes) is written from the pad's analog trigger travel (digital ZL / ZR count as a full press, values clamped to 0x7fff), and the NpadLarkType words that
  follow it (NL for the simulated NES pad). Offsets are from memory: the table entry (layout.npad_gc_trigger) is where to correct them. The older (COMPAT) layout has no trigger lifo.
- **audren performance (audren.c)**: node ids are now the ones the title put in the voice / mix / sink parameters (they were array indices); the update input's performance section
  {s32 target_node_id, 12 bytes} is parsed; with a target naming a started voice, the frame carries 0x10-byte detail entries (data source, volume ramp, mix) after its entries; a target naming a sub mix
  gets a Mix detail; detail_count and next_offset follow. Header sizes (0x10 / 0x20) were kept (not re-derivable offline).
- **bcat (bcat_core.[ch])**: full DeliveryCacheProgressImpl writer (names at +0x08 / +0x28, four s64 byte counters at +0x48..+0x60), a single DeliveryCacheDirectoryEntry packer (0x38: name, size at +0x20, MD5
  at +0x28) used by the directory reader, RFC 1321 vectors for the MD5.

## Tests
| Suite | Before | After |
|---|---|---|
| runtime test_svc_irs / test_svc_irs_sim (tools/linux-check/run-tests.sh) | 29 / 101 | 29 / 122 |
| runtime test_svc_bcat | 99 | 109 |
| input `make -f Makefile.linux test` (styles) | styles 25 | styles 31 (hid 199, bindings 28, motion 16, input 42, gesture 24, hid_sections 26 unchanged) |
| audio test_audren (clang, hwopus.c left out: libopus-dev is not installable here) | 3463 | 3481 (effects 1253, reverb3d 54, review 82, audout 516, engine 11, threads 2, speed ok) |

Scan (`.claude/skills/percent-analysis/scan.py`): measured coverage 95.6% -> 95.6% (rounded); IPC command depth row 89% -> 89%, delta +0.15 -> +0.16; irs.c impl/stub [16, 2] -> [18, 0].

## Not changed (memory not good enough to edit)
Audren performance header sizes (0x10 v1 / 0x20 v2) and the v2 selection at REV5; the processing-time model (still fixed costs); the exact PerformanceDetailType numbering beyond the entries used; the
exact GC trigger offset (0x4C20); NpadLarkType numbering; the DPD / tera state field order. All are table or constant edits for whoever has the reference in front of them.
