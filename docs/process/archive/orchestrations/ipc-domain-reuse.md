---
name: ipc-domain-reuse
batch: 2026-11-03-r11
branch: claude/ipc-domain-reuse-2026-11-03-r11
base: published squash d0d4be9 (cloud branch as pushed, NOT rebased: merge with git merge, resolve conflicts keeping round-10 code)
env: linux-cloud
status: integrated
---
# ipc-domain-reuse (brief in the session prompt; see ROUND11.md)
## Result

Outcome: Small fix. Merged commit 7af0c2b. Plain git merge of the branch (base = published squash d0d4be9/57431a7) gives hundreds of conflicts in files it never touched, so its three code commits (7918b9e, 6116122, be265cb) were cherry-picked onto main: one append-only conflict in docs/archive/changes/README.md (kept both). Never-compiled code builds clean on macOS. Small fix (1 commit): test_svc_account.c includes account.c and olsc.c in one TU and both define static async_get_result/async_cancel (renamed with #define around the olsc include), and its unlinked-user expectations predated round 10 (acc 170 and olsc 18/19/22/25/26/27 are real rows now; relaxed to 'not unknown-command'). test_ipc incl. the new domain-reuse checks passes; FIFO reuse and close error value 0x25A0A remain the author's unverified choices.
Integrator record: docs/process/archive/integrators/claude-ipc-domain-reuse-2026-11-03-r11.md
