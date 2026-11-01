---
branch: claude/gfx-stages-live-2026-11-01-r1
orchestration: gfx-stages-live
base: df2f318b56fd79d5a472ce9f3a87fb10cd10ec80
head: bf2d684d53ac0d241e78567d832147f9dd954f74  # last work commit; the integrator-file commit sits on top
env: mac
date: 2026-11-01
areas: [gfx/shader, gfx/stages, tools/gpu, docs]
status: integrated
---

# claude/gfx-stages-live-2026-11-01-r1: geometry / tessellation / transform feedback against the real titles

## Summary
- Scanned the four titles (base + latest update + DLC, every archive opened) with an extended `tools/gpu/scan_stages.py` (RomFS images, SARC, Yaz0, zstd). Only **geometry** programs exist (SMO 2 distinct, ACNH 8, MK8D 1, LAS 0); **no tess control / tess eval program and no transform feedback layout in any title**.
- Found and fixed a real translator bug those geometry programs hit: GLSLC's input vertex handles (`S2R SR_INVOCATION_INFO` + `ISBERD` + `ALD a[handle]`) were mistranslated, so every handle became 0x80000000 and the kernel read the wrong records. SMO's lightning lines all collapsed to a point. `TRANSLATOR_VERSION` 14 -> 15.
- New `t_stages_dumped`: runs a title's real dumped geometry programs through the replay (game code stays outside the repo). SMO's 3 and ACNH's 2 boot-time programs pass, and SMO's pass-through line program reproduces each input line exactly (it collapsed them before the fix).
- Evidence log lines for the stage paths (`stage path ran: ...`).
- `MultiDraw*IndirectCount` with stage programs: implemented in `mtl_stages.inc` (`stage_draw_indirect_count`). It needs a **one-line hook in `mtl_replay.m`** (gfx-msaa-views owns that file), which is not committed; see "Needs from other areas".
- **Not done**: no title *drew* a geometry program in this session's scripted runs, so nothing is verified in game. SMO's programs belong to the Odyssey travel cutscenes, New Donk City and the Ruined Kingdom, past the saves on hand. ACNH's belong to a keyboard context the name entry does not use. MK8D's is not created in boot runs. Tess parameters and the XFB layout offsets could not be read from real control sections, because no title has any; they stay *tested* (synthetic) and unproven.

## Files
`git diff --stat df2f318..HEAD`:
- `gfx/src/shader/maxwell_translate.c`: S2R 0x1d, ISBERD in geometry / tess stages, geometry ALD range guard.
- `gfx/src/mtl_shader.m`: `TRANSLATOR_VERSION` 15 (one line).
- `gfx/src/mtl_stages.inc`: `stage_ran` evidence lines, `tess_ran`, `stage_draw_indirect_count`.
- Tests: `gfx/tests/t_stages.c` (2e gated, 2f), `gfx/tests/t_stages_dumped.c` (**new**), `gfx/tests/t_xlate_stages.inc`, `gfx/tests/t_control.c`.
- `tools/gpu/scan_stages.py`, `tools/gpu/test_scan_stages.py`.
- Shared docs, append-only: `docs/architecture/gfx.md` (one paragraph in section 13, before "Rare control/system ops"), `docs/guides/gpu-title-scan.md` (new section + table appended after the b7 reference line, plus 6 words in step 3).
- `docs/archive/changes/2026-11-01-claude-gfx-stages-live-2026-11-01-r1.md` (new).
- Not touched: `mtl_replay.m`, `mtl_tex.m`, `api_texture.c`, `api_program.c`, `mtl_present.m`, `runtime/`, `orchestrations/`. `api_cmd.c` needed no change.

## Built and tested here
| Item | Status | Evidence (command + result) |
|---|---|---|
| Acceptance 1: `make -C gfx test` | tested | `make -C gfx test` -> `ALL TESTS PASSED` (t_stages ok 0 skipped, t_control ok, t_xlate ok, t_stages_dumped skipped without dump) at 4ad3f3b |
| Acceptance 2: scan table | verified (real titles) | table below; JSON in `/Users/neto/NappleWork/scan-gfxstages/json/*.json` |
| Acceptance 3: verified on a real title per stage kind used | **not verified in game**; tested with real programs | geometry is the only kind used. No scripted run drew one (see Findings). `t_stages_dumped` with SMO's and ACNH's dumped programs: `ok (0 skipped)`, 3 + 2 programs x 3 input kinds, all through the geometry stage (`stage path ran: geometry ... captured`). Tess / XFB: no title uses them -> tested only (t_stages 3, 4a-4d, t_control 4-5). |
| Acceptance 4: no regression, LAS / SMO / MK8D / ACNH 60 s boots | verified | base df2f318 vs head 4ad3f3b runtimes (head adds only 8e26ad2's sample-shading MSL, which no title uses), 60 s headless each, `MTL_DEBUG_LAYER=1`, audio off, run lock, input `30:A,40:A,50:A`, scratch save roots copied from the user's saves without shader caches (both cold). All 8 runs exit 0 with 0 skipped draws, **identical warning sets** (LAS 0, SMO 0, MK8D 2 `nvnDeviceWaitForError`-type, ACNH 28 unknown-NVN-name lookups + 2 known), no Metal validation lines. Frames base -> head: LAS 3467 -> 3552, SMO 2245 -> 2594, MK8D 3460 -> 3553, ACNH 1727 -> 1781. Screenshots at frames 1000 / 2000 / 3000: the same scenes; the large pixel differences are timing (the inputs fire at fixed seconds), rendered correctly in both. Logs: `/Users/neto/NappleWork/logs/gfxstages-reg-{base,head}-<titleid>/` |
| GLSLC handle fix catches the bug | tested | `t_stages` 2f fails on the old translator (`16 words differ`; the 0x80000000 handle read vertex 0 of another record) and passes on the fix; `t_stages_dumped` on SMO: old translator captures every line as (v0, v0), fixed captures (v0, v1) |
| `stage_draw_indirect_count` | tested with the hook applied locally (not committed) | `mtl_replay.m` hook + `make -C gfx build/tests/t_stages EXTRA_CFLAGS=-DNAPPLE_TEST_IND_COUNT_STAGES` -> `t_stages: ok`, log `MultiDraw*IndirectCount with transform feedback / geometry / tessellation: count and arguments read at encode time (2 of 3 draws)`; without the hook the same test FAILs (draws without the GS) |
| scan_stages.py | tested + run on real titles | `python3 -I tools/gpu/test_scan_stages.py` OK (6 tests; also under python3.14) |

### Scan results (acceptance 2)
| Title | Inputs scanned | Programs (distinct vs / fs / cs) | Geometry (distinct) | Tess ctrl / eval | XFB | Location |
|---|---|---|---|---|---|---|
| LAS 01006BB00C6F0000 | v0 RomFS | 6429 (2881 / 2858 / 26) | 0 | 0 / 0 | 0 | - |
| SMO 0100000000010000 | v0 RomFS (all 4335 compressed files opened) + 120 s boot dump | 13910 (5045 / 8107 / 68) | 2 (boot: 3 hashes `fccb3f42c7f70682` 2-vtx line strip, `ab99b39df97424a4` / `d68a01153da65dbd` 64-vtx line strip) | 0 / 0 | 0 | `ShaderData/alThunderShader.szs` |
| MK8D 0100152000022000 | v1376256 patched RomFS + Booster DLC RomFS | 36275 (10402 / 13239 / 24) | 1 (code 0x300) | 0 / 0 | 0 | `Common/Gfx/gsys_resource.Nin_NX_NVN.release.sarc` |
| ACNH 01006F8002326000 | 3.0.3 patched RomFS + 3 DLC RomFS + 120 s boot dump | 9720 (2533 / 3505 / 56) | 8 (boot: `8c8302af6b2a4b88`, `c00085a7efde947a`, 6-vtx triangle strip) | 0 / 0 | 0 | `Swkbd/shader/*OceanShader*_{Nx,Win}.arc.szs` |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt; everything was compiled and run on this Mac.
- `stage_draw_indirect_count` (mtl_stages.inc, da20e18) is dead code until the `mtl_replay.m` hook lands, and `__attribute__((unused))` keeps -Werror quiet.
- `t_stages` 2e (`test_gs_indirect_count`) is compiled only with `-DNAPPLE_TEST_IND_COUNT_STAGES`.
- The geometry path has never run in a game. Only real programs with synthetic input (`t_stages_dumped`).

## Findings
- **Translator bug (fixed)**: SR_INVOCATION_INFO (0x1d) answered 0, and ISBERD in a geometry program was read as an attribute address. Every GLSLC geometry program reads its inputs through that prologue (SMO's and ACNH's do; MK8D's has the same size as SMO's pass-through program and probably the same prologue). So before this branch any game geometry draw read wrong records. Decisions: INVOCATION_INFO = vertex count << 16 | invocation for geometry (yuzu answers 0x00ff0000; the real count is what the hardware gives, and GLSLC compares k against it); patch vertices << 16 for tess (as yuzu); ISBERD -> vertex operand modulo the vertex count in geometry (the handle of instance i is i * count + k), a plain copy in tess; ALD vertex operand >= count reads 0 (the hardware's "no vertex").
- **Where the programs are**: SMO's thunder programs are referenced by `DemoWorldMove{Forward,Backward}{,Arrive}StageDesign` (`RequestThunderVtxNum`, `RequestThunderJudgeInterval`), `DistantDemoWorldMoveThunder` (Odyssey travel cutscenes), `CityWorldHomeStageMap` (EffectObjThunderRunbling), `BossRaidWorldHomeStageDesign` and the MoonWorldKoopa stages. That is inferred from stage data, not seen drawn. The SMO save on hand loads into Cap Kingdom at night. A fresh save reaches the airship intro (daylight) within 120 s. Neither binds them. ACNH's ocean programs sit in its own keyboard folder (`Swkbd/shader`). The fresh-save name keyboard (reached at ~100 s, screenshot frame 3000) does not bind them, so another keyboard theme does.
- **How a human verifies in game**: SMO, any save with the Odyssey: travel to another kingdom (the travel cutscene), with `NAPPLE_GFX_CTL_DUMP` off and the log on. Expect `stage path ran: geometry, program ab99b39df97424a4 ...` and lightning bolts drawn as lines. Before this branch they were invisible (zero-length).
- `scan_stages.py`'s first version opened only uncompressed files and shader-named paths: MK8D's course / driver model archives carry ~55% of its programs. Every archive is now opened by default.
- The MK8D DLC (Booster Course Pass v65536) RomFS is 2 files (an unlock), and the content is in the update.
- **gfx-msaa-views' sample-shading translator patch landed here** (8e26ad2; their message offered it because `shader/` is this branch's area): with sample shading, frag coord x/y = floor(in.pos) + `get_sample_position(sample_id)`, and varyings use `[[sample_perspective]]` / `[[sample_no_perspective]]`. It is covered by `TRANSLATOR_VERSION` 15. Its behaviour test `t_sample_shading.c` is on their branch (they report 3.375 / 3.875 / 3.125 / 3.625 with the patch, 3.5 without). Here `make -C gfx test` passes with it (t_msaa, t_xlate). If both branches carry the patch, keep one copy (same text).
- MK8D: a 120 s run with A every 1.5 s reaches a Grand Prix race (Mario Kart Stadium, frame 6000; `/Users/neto/NappleWork/logs/gfxstages-mk8d-race`) and creates **no** geometry program, so the gsys one is not used in a plain race.
- ACNH has two data dirs in the shared work dir (`7fc1baff976aeca4` base, `ff1d1c05670db602` 3.0.3), as expected for the update build; `boot_test.sh` picks the newest (3.0.3).

## How to integrate
- No ordering constraint. If gfx-msaa-views lands too: apply the hook below to `mtl_replay.m` after both are merged, then drop the `#ifdef NAPPLE_TEST_IND_COUNT_STAGES` guards in `gfx/tests/t_stages.c` (two places) and the `__attribute__((unused))` on `stage_draw_indirect_count`.
- Hook (one line, top of `do_draw_indirect_count` in `gfx/src/mtl_replay.m`; `mtl_stages.inc` is included above it, so no forward declaration is needed):
  ```diff
   static void do_draw_indirect_count(const p_draw_ind_count *d) {
  +    if (stage_draw_indirect_count(d)) return; /* transform feedback / geometry / tessellation: count and records read at encode time (mtl_stages.inc) */
       int native;
  ```
- Conflict hotspots: `gfx/src/shader/maxwell_translate.c` (the msaa sample-shading patch is already in 8e26ad2: if their branch also carries it, the hunks are identical). `gfx/src/mtl_shader.m` `TRANSLATOR_VERSION` line: take the higher number, or bump once more. `docs/architecture/gfx.md` section 13: keep both paragraphs.
- Build: `make -C gfx test` (expect `ALL TESTS PASSED`), `python3 -I tools/gpu/test_scan_stages.py` (OK), then `make -C runtime`.
- No Linux-only calls. `scan_stages.py` is Python 3 standard library only; zstd works when run with python3.14.

## How to test
1. `make -C gfx test`: `ALL TESTS PASSED`. `t_stages: ok (0 skipped)` includes 2f. A failure on `ISBERD handles select input vertices 1 and 0` means the translator fix is missing.
2. Real programs: `NAPPLE_GFX_CTL_DUMP=<dir>` on a 60 s SMO boot (mkdir it first; under the run lock), then `NAPPLE_GFX_STAGE_DUMP=<dir> MTL_DEBUG_LAYER=1 gfx/build/tests/t_stages_dumped` -> `3 geometry programs run, 9 (program, primitive) runs emitted vertices`, `t_stages_dumped: ok (0 skipped)`, and `stage path ran: geometry` lines. Ready-made dumps (SMO / ACNH geometry programs only): `/Users/neto/NappleWork/logs/gfxstages-smo-gs`, `/Users/neto/NappleWork/logs/gfxstages-acnh-gs`.
3. Hook check, after applying it: `make -C gfx build/tests/t_stages EXTRA_CFLAGS=-DNAPPLE_TEST_IND_COUNT_STAGES && MTL_DEBUG_LAYER=1 gfx/build/tests/t_stages` -> `t_stages: ok`.
4. Scanner: `python3 -I tools/gpu/test_scan_stages.py` -> OK. A real scan, for example LAS: `target/release/napple build --dump-romfs <LAS nsp> -k <keys> -o /tmp/las.romfs && python3.14 -I tools/gpu/scan_stages.py scan /tmp/las.romfs --title-id 01006BB00C6F0000` -> `6429 programs`, no gs (delete the 6 GB image afterwards).
5. Regression guard: the 60 s boots of the four titles (above). Because of the `TRANSLATOR_VERSION` bump, the first run per title recompiles its shader cache (fewer frames on that first run).
6. Human: SMO with a save that has the Odyssey; travel between kingdoms and look for lightning lines in the cutscene, with `stage path ran: geometry` in the log.

## Fix policy hints
- Safe to fix directly: `TRANSLATOR_VERSION` collisions (bump), test-guard removal once the hook is in, doc wording.
- Needs a decision: if a game shows geometry output where vertex handles from INVOCATION_INFO * count + k exceed the count for instanced GS, the modulo mapping is the thing to revisit (alternative: yuzu's plain copy with INVOCATION_INFO low byte 0).

## Needs from other areas
- gfx-msaa-views (`mtl_replay.m`): the one-line `stage_draw_indirect_count` hook above.
- Anyone with a later-game SMO save, or an ACNH save that opens the ocean keyboard: an in-game verification run (see How to test 6).

## Integration result
- Date: 2026-11-01. Merged into local `main` as 5b0cf26 (`--no-ff` merge of 255319f, after sweep-small and media-live, before gfx-msaa-views as the user asked: 8e26ad2 carries gfx-msaa-views' sample-shading translator patch). Outcome: **Clean** (no `integrate:` fixes at merge time; no native ports: the translator is portable on purpose and `scan_stages.py` is plain Python).
- Tests on this Mac (scratch worktree, same tree as the merge): `make -C gfx -j6 test` -> ALL TESTS PASSED (t_stages ok 0 skipped incl. 2f, t_control ok, t_xlate ok, t_msaa ok, t_stages_dumped skipped without a dump); `python3 -I tools/gpu/test_scan_stages.py` OK (6 tests).
- Real programs (How to test 2) with the ready-made dumps: SMO `3 geometry programs run, 9 (program, primitive) runs emitted vertices`, ACNH `2 geometry programs run, 6 ...`, both `t_stages_dumped: ok (0 skipped)` with `MTL_DEBUG_LAYER=1` and `stage path ran: geometry` lines for fccb3f42c7f70682 / ab99b39df97424a4 / d68a01153da65dbd and 8c8302af6b2a4b88 / c00085a7efde947a.
- The `mtl_replay.m` hook for `stage_draw_indirect_count` and the four-title regression boots are recorded below (after gfx-msaa-views landed).
