# 2026-11-01 claude/nvn-object-layouts-2026-11-01-r3: guest-visible NVN objects within the titles' object sizes

Batch 2026-11-01-r3, orchestration `nvn-object-layouts` (follow-up to the r2 program-hash bug, 30e67b4). Mac (Apple M5), local only.

## What changed
- **`gfx/src/obj.h`**: `NVN_MIN_<type>` for all 30 guest-visible object types. Each is the smallest size measured across SMO 3.5.1, MK8D 0.12.16 (32-bit), LAS 8.2.0 and ACNH 20.5.4: the minimum of nvn.h's reserved size and the smallest credible gap to a neighbouring object.
  - `FITS(T, H)` now asserts `sizeof(T) <= NVN_MIN_H <= sizeof(H)`.
  - The table and its evidence are in `docs/architecture/gfx.md` section 14.
- **`prog_t`** is now magic, nstage and hash[0..3] (0x28 bytes, was 0x98 of which r2 wrote 0x88).
  - The new evidence: in Animal Crossing, a live NVNprogram (magic intact) has an NVNbuffer at +0x80 and a memory pool at +0xb0. Its program object is at most 0x80 bytes, and r2's layout still zeroed +0x80..0x88, the first 8 bytes of that buffer, at Initialize and SetShaders.
  - The `NVNshaderData sd[6]` copies were written but never read, so they are removed.
  - All six stage hashes stay in the host table keyed by program address (`gfx_program_hashes`). Stages 0..3 are mirrored in the object for programs the guest copies by value.
  - `NVN_MIN_NVNprogram` is 0x80.
- **`NAPPLE_GFX_OBJTRACE=1`** (`gfx/src/api_objtrace.c`, with hooks in every Initialize, Finalize and SetDefaults of `api_*.c`): measures object sizes in a running title.
  - It reports per type the gaps to the nearest live NVN object (an upper bound of the title's object size, the exact size in arrays).
  - Excluded from the gaps: objects on the calling stack (temporaries in 64-bit titles), objects set up again without Finalize, and objects whose first word changed (freed without Finalize).
  - A neighbour inside nvn.h's reserved size is logged once as `objtrace close:`.
  - Off by default: one predicted branch per lifecycle call.
- **`gfx/tests/t_obj_canary.c`**: each of the 30 families runs in a block with a canary right after `NVN_MIN_<type>` bytes. It calls SetDefaults or Initialize, then all 328 entry points whose first parameter is a non-const pointer to the object (Finalize last), and fails with the name of the first call that writes into the canary.
  - The call lists are generated from `nvn_protos*.h` by `gfx/tests/t_nvn_canary_gen.py`.
  - The test catches overruns: with the minimums halved, it fails on the expected calls (QueueBuilder, Buffer, Texture, TextureView, Sampler, pools, ...).
- **`gfx/tests/t_nvn_ext_prog.c`**: updated for the new `prog_t`. The neighbour now sits at `NVN_MIN_NVNprogram`, and the scribble hits the fragment hash at `prog_t.hash[1]`.

## Findings
- Every other host layout already fit. The tightest fits sit exactly at nvn.h's size: `samp_t` (0x30; the titles show ≤ 0x78), `dpool_t` (0x20; three titles place a sampler pool exactly 0x20 after the texture pool), the queue and window builders (0x40), `view_t` (0x10) and the state objects.
- MK8D runs as a 32-bit title, so its stack temporaries cannot be recognised and its texture readings are noise. The `NVNtexture` minimum (0x80) rests on ACNH (0x88 to a pool, 22,498 times) and nvn.h.
- Builders, views and most state objects are stack locals. Their "recent objects" pairing mixes frames, so only array strides (vertex attrib 4, vertex stream 8, consecutive color, channel mask and polygon states 4) count as evidence. The other builder minimums are nvn.h's.
- One SMO traced run crashed at 0.84 s in the title's SDK (SIGBUS on uncommitted guest memory, `objlay-smo-trace2`). The traced rerun and an untraced rerun were clean, as were all earlier SMO runs. The crash is outside gfx.

## Verified
`make -C gfx test`: ALL TESTS PASSED (t_obj_canary 328 calls, t_nvn_ext_prog ok). Traced 90 s boots of all four titles, plus final untraced 90 s boots: see the integrator file.

## Status
See `integrators/claude-nvn-object-layouts-2026-11-01-r3.md`.
