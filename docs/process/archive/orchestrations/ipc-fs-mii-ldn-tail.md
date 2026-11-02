---
name: ipc-fs-mii-ldn-tail
batch: 2026-11-03-r11
branch: claude/ipc-fs-mii-ldn-tail-2026-11-03-r11
base: main (local session restarted after cloud credits ran out)
env: mac
model: claude-sonnet-5-5
status: integrated
session: local_f85e99a7-29dc-4c9a-9ca2-81cda9b46f58
---
# ipc-fs-mii-ldn-tail: last local IPC rows in fs, mii image and ldn; ncm offset fix (brief in the session prompt; see ROUND11.md)
## Result

Outcome: Clean (landed by cherry-pick). Merged commit b3e59bb. The branch is based on the published squash f1206c1 (history unrelated to main's round-7..10 merges): a plain git merge gave add/add conflicts in fs.c, ldn*.c, mii_img.c and tests, so its six commits (396f205 5d39d7b cef0b61 ec5efea fb37007 a9c876f) were cherry-picked onto main without conflicts (equivalent to a rebase; the session branch is untouched). Runtime suites: test_svc_fs_local, test_svc_local_r10_ldn (207), test_svc_mii_img (106), test_svc_ncm (146) pass; gfx 0 skipped; risk-title 158/0.
Integrator record: docs/process/archive/integrators/claude-ipc-fs-mii-ldn-tail-2026-11-03-r11.md
