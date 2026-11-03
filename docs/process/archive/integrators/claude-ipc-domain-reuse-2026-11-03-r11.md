# Integrator note: claude-ipc-domain-reuse-2026-11-03-r11

Branch: `claude/ipc-domain-reuse-2026-11-03-r11` (from `main`). Change note: `docs/archive/changes/2026-11-03-ipc-domain-reuse-2026-11-03-r11.md`.

## Summary
1. `runtime/src/ipc.c`: closed domain object ids are reused (never-used ids first, then FIFO of freed ids); closing an unknown id answers an error result instead of aborting; full only at 512 open.
2. `runtime/tests/test_svc_account.c`: tests for an unlinked user (account manager and olsc rows). No code fix was needed; `0x1907C` is not produced anywhere today (documented).

## Files
- `runtime/src/ipc.c`, `runtime/tests/test_ipc.c`, `runtime/tests/test_svc_account.c`, the change note and this file.

## Tests run (Linux)
- `test_ipc.c`: built with gcc `-fsanitize=address,undefined` against `runtime/src/ipc.c` (included by the test) plus a throwaway stub file outside the product tree for handle table / log / kernel symbols. Result: all new domain checks pass (6700+ checks). The stub's handle table does not reuse slots like the real one, so the one pre-existing check "stale handle stays invalid after slot reuse" fails on Linux only; it is a stub artifact.
- `tools/linux-check/run-tests.sh runtime/tests/test_svc_account.c runtime/tests/test_svc_olsc.c`: 150 and 23 checks, 0 failed.
- `-fsyntax-only` of test_ipc.c with `tools/linux-check` compat headers: clean.

## Unbuilt (never compiled on macOS)
- `runtime/src/ipc.c` changes and `runtime/tests/test_ipc.c` additions in the real runtime build. The new test code uses only portable C and the static helpers `domain_alloc_locked` / `domain_release_locked`.

## Mac build/test
```bash
make -C runtime GFX=napple test test-services test-ipc-reference
```
Check on Mac: `test_ipc` passes including the old handle-table checks (they use the real kernel handle table).

## Decisions / risks
- FIFO reuse instead of lowest-free (see change note). Close-error value `0x25A0A` (module 10, desc 301) is unverified.

## Integration result
- Date: 2026-11-03 (batch r11 integrator run, local main, unpushed)
- Merged commit: 7af0c2b
- Outcome: Small fix
- Plain git merge of the branch (base = published squash d0d4be9/57431a7) gives hundreds of conflicts in files it never touched, so its three code commits (7918b9e, 6116122, be265cb) were cherry-picked onto main: one append-only conflict in docs/archive/changes/README.md (kept both). Never-compiled code builds clean on macOS. Small fix (1 commit): test_svc_account.c includes account.c and olsc.c in one TU and both define static async_get_result/async_cancel (renamed with #define around the olsc include), and its unlinked-user expectations predated round 10 (acc 170 and olsc 18/19/22/25/26/27 are real rows now; relaxed to 'not unknown-command'). test_ipc incl. the new domain-reuse checks passes; FIFO reuse and close error value 0x25A0A remain the author's unverified choices.
