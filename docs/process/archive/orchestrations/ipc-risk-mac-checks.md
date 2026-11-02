---
name: ipc-risk-mac-checks
batch: 2026-11-03-r11
branch: claude/ipc-risk-mac-checks-2026-11-03-r11
base: orchestrate/2026-11-03-r11
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: pending
launched_at: 2026-11-03
---

# ipc-risk-mac-checks: exercise the recent IPC risk rows on the real titles

## Owned paths
`runtime/tests/test_risk_title.c` (new), `tools/golden/risk/` (new), `runtime/Makefile` (append one rule)

## Do not touch
Everything else (other sessions edit runtime/src/services, runtime/src/net, gfx shader/replay/stages, media in parallel), `orchestrations/`.

## Work and acceptance
1. Applet ExecuteProgram: a macOS restart fixture that proves the execv path restarts the runtime with the new program args (headless, under the run lock).
2. fs permission bits: check the fs.c permission checks against the real `main.npdm` of all four titles (a test that loads each title's npdm from the shared data dir and asserts the title's own saves/romfs opens succeed and a forbidden mount fails).
3. Unlinked-user account result 0x1907C: a test that drives acc with no linked Napple profile.
4. Four-title boots re-checked: rc 0, missing_ipc []. Report any row that misbehaves as a bug in the integrator file; fix only trivial ones.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data read-only, lockf run lock, local-only, never ask, never push). Model: Sonnet 5.5, medium effort: work test-first in small steps, commit each.

## Result

Outcome: Clean. Merged commit 075b7dc. No fixes needed. make -C runtime test-risk-title: 158 checks 0 failed 0 skipped; runtime test/test-services/test-ipc-reference green (missing 0); gfx test green. Note: the PATH needs the rustup toolchain bin for the runtime test-net-room cargo step.
Integrator record: docs/process/archive/integrators/claude-ipc-risk-mac-checks-2026-11-03-r11.md
