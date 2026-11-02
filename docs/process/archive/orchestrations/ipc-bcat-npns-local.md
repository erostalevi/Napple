---
name: ipc-bcat-npns-local
batch: 2026-11-03-r11
branch: claude/ipc-bcat-npns-local-2026-11-03-r11
base: published squash d0d4be9 (cloud branch as pushed, NOT rebased: merge with git merge, resolve conflicts keeping round-10 code)
env: linux-cloud
status: integrated
---
# ipc-bcat-npns-local (brief in the session prompt; see ROUND11.md)
## Result

Outcome: Small fix + semantic conflict resolution. Merged commit 2a11c87. Landed by cherry-picking its two code commits (cd6c6cc, a10e5ab) onto main (see ipc-domain-reuse for why a plain merge is not workable). Conflicts, resolved keeping both behaviours: bcat.c command table (main already had 20101/20300/20301/20400/20401/90200 real from round 10; its 7 stub rows were dropped in favour of the branch's 20410/20500/30210/30300/90202/90300/90301 implementations); push.c includes and npns:u table (branch's local state serves 5/103/104/111/151/153, round-10 daemon state keeps 7/152/101/102/120/154; npns_local_set_state now fires the round-10 state-change event list and npns_local_reset also clears it) and test_svc_push.c (both test blocks kept). Fix commit: test_svc_local_r10_bcat.c / _npns.c asserted those rows stay stubs; now assert IMPL. Previously never-compiled code builds clean; test_svc_bcat 227/0, test_svc_push 88/0. Layouts (timer u64 id + u32 seconds, push log 0x18, usage 0x10, stats 0x10, JID buffer) remain the author's unverified guesses.
Integrator record: docs/process/archive/integrators/claude-ipc-bcat-npns-local-2026-11-03-r11.md
