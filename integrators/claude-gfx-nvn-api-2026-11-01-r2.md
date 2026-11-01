---
branch: claude/gfx-nvn-api-2026-11-01-r2
orchestration: gfx-nvn-api
base: 79415f0
head: cbe70a6 (code at 30e67b4; the integrator-file commit follows)
env: mac
date: 2026-11-01
areas: [gfx, runtime/a32 (generated table only), tools]
status: ready
---

# claude/gfx-nvn-api-2026-11-01-r2: the 14 missing NVN entry points, refused MSAA branches, ACNH program hash

## Summary
- 14 NVN names added to `names.txt` with prototypes and implementations in the same commit (6c0d0a9): NVN 537 of 537. Parameter lists inferred from the
  neighbouring entry points (no public header has them); an xref scan shows Animal Crossing loads the 13 and never reads the pointers back.
- `tools/gen_a32_nvn.py` reads `nvn_protos_ext.h` too; `a32_nvn_sigs.inc` regenerated: 537 rows (was 477).
- Refused copy / Downsample branches replaced by a generic copy pass `xcopy_draw` (438ae83): depth <-> colour, multisampled pairs of another format / size /
  sample count, 3D with a format or size change, integer <-> float; draw copies now cover all layers / depth slices of the region.
- ACNH "program hash 0x3": a host layout bug (diagnosed on the title: guest object <= 0x88 bytes, its neighbour wrote 0x3 into prog_t.hash[4]); stage hashes
  now kept host-side, nothing written past +0x88 of a program object (30e67b4).
- SetProvokingVertexMode: mode recorded and reported once; translator support requested (below).

## Files
`git diff --stat 79415f0..HEAD`: `gfx/src/nvnapi/{names.txt,nvn_protos_ext.h}`, `gfx/src/{api_cmd.c,api_device.c,api_program.c,api_texture.c,api_state.c,mtl_replay.m}`,
`tools/gen_a32_nvn.py`, `runtime/src/a32/a32_nvn_sigs.inc` (regenerated only), new `gfx/tests/{t_nvn_ext.c,t_nvn_ext_prog.c,t_msaa_copy.c}`,
`docs/archive/changes/2026-11-01-claude-gfx-nvn-api-2026-11-01-r2.md`, this file. No shared files; `gfx/Makefile` untouched (tests are picked up by wildcard).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acc. 1: 14 names + implementations, same commit, tests | tested | 6c0d0a9; `t_nvn_ext`: all resolve, BindTexturesDeferred draw reads 3e800000, builder view offset == texture view offset, getters; `NVN: 537 of 537 entry points implemented` |
| Acc. 2: a32 signatures; MK8D message gone | verified | 537 rows; MK8D 90 s (`gfxnvn-r2-mk8d`): no "no host implementation or signature", no WaitForError line (resolved, never called) |
| Acc. 3: no `NVN lookup of unknown name` in ACNH / SMO / MK8D | verified | none in `gfxnvn-r2-{acnh,smo,mk8d,las}/run.log` (r1 runs had 2 in ACNH, 2 in MK8D) |
| Acc. 4: refused MSAA branches with readback tests | tested | `t_msaa_copy` 7 cases (int->float Downsample, 4x depth -> R32F / Depth16, 4x R32F -> depth, 4x RGBA8 -> 4x BGRA8 copy, 4x -> 1x copy, depth <-> R32F copies, 3D slices -> BGRA8 array) under MTL_DEBUG_LAYER=1; no title hits these branches |
| Acc. 5: ACNH program hash 0x3 | verified (fixed) | diagnosis run: `stage hash 4 (object offset 0x88) holds 0x3, SetShaders stored 0`; after the fix ACNH 90 s has no "program hash" line; `t_nvn_ext_prog` |
| Acc. 6: four-title 90 s boots | verified | ACNH / MK8D / SMO / LAS: exit 0, 0 skipped draws, 0 unimplemented calls; only warning is ACNH's pre-existing "texture slot 0 ... type 5 but type 2" (present in every r1 run); screenshots normal |
| `make -C gfx test` | tested | ALL TESTS PASSED (t_sample_shading now 0 skipped with the r1 translator fix on main) |

Logs: `/Users/neto/NappleWork/logs/gfxnvn-r2-{acnh,mk8d,smo,las}/` (run.log, shots, the runtime binary).

## Unbuilt / untested list
- None unbuilt. Not reached by any title: the 14 entry points' bodies (except lookups), the xcopy_draw paths.

## Findings
- Animal Crossing's NVNprogram is at most 0x88 bytes. Before this fix, a compute program's hash (stage 5, +0x90) written by SetShaders would also have corrupted the
  neighbour object. Other guest objects with host layouts near their reserved size may have the same risk (not audited: obj.h / other areas).
- Provoking vertex: Metal offers no switch; nothing in the four titles sets a non-default mode (no log line).
- `nvnDeviceWaitForError` is resolved by MK8D but never called in 90 s.

## How to integrate
- No ordering constraints in r2 as far as known. `mtl_replay.m` is also near gfx-shader-views' area (they own mtl_tex.m / mtl_priv.h / shader): my edits
  are confined to the copy / resolve functions (`xcopy_draw` inserted before `copy_t2t_draw`), so conflicts, if any, are textual.
- Build: `make -C gfx -j4 test`; `make -C gfx lib adapter && make -C runtime` (the a32 table is compiled into the runtime).
- `python3 tools/gen_a32_nvn.py > runtime/src/a32/a32_nvn_sigs.inc` must be re-run whenever `nvn_protos*.h` change.

## How to test
1. `make -C gfx -j4 test` -> ALL TESTS PASSED; `t_nvn_ext`, `t_nvn_ext_prog`, `t_msaa_copy` print ok.
2. ACNH / MK8D / SMO headless 60-90 s: `grep -c 'NVN lookup of unknown name\|no host implementation\|program hash' run.log` -> 0.
3. Regression: LAS / SMO / MK8D / ACNH exit 0, 0 skipped draws.

## Fix policy hints
- Wrong signature reported later for one of the 14 (a title calls it and crashes or misbehaves): fix the prototype in `nvn_protos_ext.h` + implementation, rerun
  the a32 generator. Safe direct fix.
- A Metal validation abort in `xcopy_draw`: safe to return 0 for that pair (restores the old refusal).

## Needs from other areas
- Translator (gfx-shader-views / shader owner): a ShaderKey bit for the provoking vertex mode (flat inputs taken from the first vs last vertex) if a title ever sets it.
- obj.h owner: consider shrinking guest-visible host layouts (prog_t and others) to the smallest SDK's reserved sizes; prog_t now only uses up to +0x88 by convention (static assert in api_program.c).
