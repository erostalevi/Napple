---
branch: claude/nvn-api-tail-2026-11-03-r11
orchestration: nvn-api-tail
base: 281f4d3
head: PENDING
env: mac
date: 2026-11-03
areas: [gfx]
status: ready-with-caveats
---

# claude/nvn-api-tail-2026-11-03-r11: NVN tail entry points (nvnTextureBuilderGetViewOffset, nvnDeviceWaitForError, nvnProgramGetSubtileSize)

## Summary
Audit result: all three entry points are already real implementations on the base commit (r2 work), not inferred stubs, and each has a test in gfx/tests/t_nvn_ext.c:
- nvnTextureBuilderGetViewOffset (gfx/src/api_texture.c:226) shares view_offset() with nvnTextureGetViewOffset; test compares builder vs texture on a mipmapped 2D array.
- nvnDeviceWaitForError (api_device.c:125) returns 0 (no error), counts calls; tested.
- nvnProgramGetSubtileSize (api_program.c:191) reports 0x0; tested.
Names are in names.txt, prototypes in nvn_protos_ext.h. gfx reports "NVN: 537 of 537 entry points implemented". tools/gen_a32_nvn.py regenerated: a32_nvn_sigs.inc is byte-identical (no diff). No code change was needed; decision: do not invent changes. Not done: four-title boot check (not run; all names resolve, 537/537, so no 'unknown name' lookups are expected for these).

## Files
No source changes. Only this integrator file.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 3 entry points real + tested | tested | t_nvn_ext.c covers them |
| make -C gfx test | verified | ALL TESTS PASSED |
| a32 sigs regenerate | verified | gen_a32_nvn.py, git status clean |
| four-title boots w/o unknown NVN names | untested | not run |

## Unbuilt / untested list
None.

## Findings
Orchestration item premise (maybe stubs) was already satisfied by r2.

## How to integrate
Docs-only; no ordering constraints.

## How to test
make -C gfx test -> ALL TESTS PASSED.

## Fix policy hints
n/a

## Needs from other areas
None.
