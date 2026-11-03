# claude-ipc-fs-mii-ldn-tail-2026-11-03-r11

Branch: `claude/ipc-fs-mii-ldn-tail-2026-11-03-r11` (local only, not pushed). Change note: `docs/archive/changes/2026-11-03-ipc-fs-mii-ldn-tail-2026-11-03-r11.md`.

## Rows converted
| Service | Cmd | Test |
|---|---|---|
| fsp-srv | 84 ListAccessibleSaveDataOwnerId | `test_svc_fs_local` (list, start index, other program, short buffer, short input) |
| miiimg | 18, 19, 20 migration store | `test_svc_mii_img` (stage, clear, save, full, replace, reset) |
| ldn | 503 RecvActionFrame | `test_svc_local_r10_ldn` (push, FIFO, bounds, cut buffer, filter by id, blocking wake/timeout) |

Kept as documented default: btm 2 (UUID layout not public). Bug fix: ncm ContentInfo type at 0x16 (`test_svc_ncm`).

## Decisions
- 84 input layout and 503 reply layout are host-chosen (pages do not give offsets); marked in comments.
- ldn 503 empty answer stays `HZ_R_LDN_NETWORK_NOT_AVAILABLE` so the existing r10 test holds.

## Built and tested
`make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0, missing 0 (a fresh worktree needs the rustup toolchain bin on PATH for napple-room, and the gfx/frontend/input/audio libs built first).

## Risks
- `ldn_push_action_frame` has no caller yet: `runtime/src/net/` (another session) must call it from the room/provider receive path.
- A blocking 503 sleeps the service thread up to 200 ms.
- The migration store is memory only.

## Boots (verified)
Headless, `--seconds 60`, private `--save-root`, under `lockf -k /Users/neto/NappleWork/.run.lock`: 0100000000010000, 01006BB00C6F0000, 0100152000022000 reach `notify-running`;
01006F8002326000 was still loading (`module-init`, IStorage reads) at the 60 s limit, no crash. All four rc 0, `missing_ipc []`, stub list `nvdrv:13` only. None of the titles calls fs 84,
miiimg 18-20 or ldn 503, so those rows are unit-tested only.

## Integration result
- Date: 2026-11-03 (batch r11 integrator run, local main, unpushed)
- Merged commit: b3e59bb
- Outcome: Clean (landed by cherry-pick)
- The branch is based on the published squash f1206c1 (history unrelated to main's round-7..10 merges): a plain git merge gave add/add conflicts in fs.c, ldn*.c, mii_img.c and tests, so its six commits (396f205 5d39d7b cef0b61 ec5efea fb37007 a9c876f) were cherry-picked onto main without conflicts (equivalent to a rebase; the session branch is untouched). Runtime suites: test_svc_fs_local, test_svc_local_r10_ldn (207), test_svc_mii_img (106), test_svc_ncm (146) pass; gfx 0 skipped; risk-title 158/0.
