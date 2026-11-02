---
name: nvdrv-gpu-routing
batch: 2026-11-03-r11
branch: claude/nvdrv-gpu-routing-2026-11-03-r11
base: published squash d0d4be9 (cloud branch as pushed, NOT rebased: merge with git merge, resolve conflicts keeping round-10 code)
env: linux-cloud
status: integrated
---
# nvdrv-gpu-routing (brief in the session prompt; see ROUND11.md)
## Result

Outcome: Semantic conflicts resolved (keep both). Merged commit 210b461. Landed by cherry-picking its two commits (7e2c257, 1d806bc) onto main. The branch did not know the round-10 nvdrv-public-layouts work, which already implements 'H' 0x17 GET_ERROR_NOTIFICATION and an EVENT_ID_CONTROL (0x12, {cmd,event_id}); resolution: round-10 code, layouts and docs kept as authoritative; added from the branch only its genuinely new rows, as separate cases: 'H' 0x1D EVENT_ID_CTRL (own form {cmd,event_id,fd,pad}, event_id 0..2, shares the round-10 enable mask, clear keeps it), 'G' 0x0C GET_TPC_EXCEPTION_EN_STATUS, 'G' 0x0D NUM_VSMS (constants added to nvdrv_nodes_core.h), their tests (event_mask renamed to events), and a 'Round 11 additions' section appended to docs/reference/nvdrv-layouts.md (the branch's version of that file was an add/add conflict and its 'stay NotImplemented' table is superseded). Its layouts are proposed from memory, unverified (0x1D in particular may not be a real Switch number). nvdrv.c unchanged. test_svc_nvdrv_nodes 231/0, all runtime suites green.
Integrator record: docs/process/archive/integrators/claude-nvdrv-gpu-routing-2026-11-03-r11.md
