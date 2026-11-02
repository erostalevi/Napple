---
name: error-applet-output
batch: 2026-11-02-r6
branch: claude/error-applet-output-2026-11-02-r6
base: orchestrate/2026-11-02-r6
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_3b3cd256-1668-4a2b-8542-c3387e1fbb05 (reused)
launched_at: 2026-11-02
---

# error-applet-output: the error applet (and other library applets) return the output storage the SDK expects

## Goal
net-host-provider found: MK8D Online Play with `NAPPLE_NET=host` is refused an account token (expected without a Napple/online account), shows the error applet, then calls svcBreak because the runtime's error applet pushes no output data. On a console the error applet returns (for the modes that have one) an output storage, or closes with the documented exit; the SDK then returns to the game normally. Fix the error applet's output/exit contract generically for every error-applet mode (ShowError, ShowSystemError, ShowApplicationError, ShowErrorRecord, ShowEula...), and audit the other library applets' output contracts for the same gap.

## Owned paths
`runtime/src/services/applet*.c/.h`, `applet_lib_core.[ch]` (error applet + other library applets' output), `runtime/tests/test_svc_applet*.c`, own change note.

## Do not touch
`runtime/src/net/`, `offline*.c`, `online_core.c`, `account*.c`, `crates/`, `gfx/`, `orchestrations/`.

## Acceptance
1. Error applet modes push the output / exit the SDK expects (evidence: disassembly of the SDK's `nn::err` / `nn::la` readers in MK8D's sdk module, addresses in the change note; or switchbrew); unit tests.
2. **Verified**: MK8D update, Online Play with `NAPPLE_NET=host` (build the runtime from your branch merged with `claude/net-host-provider-2026-11-02-r6` in a scratch worktree for this check only): the error applet shows and the game returns to its menu, no svcBreak, rc 0.
3. Other library applets audited; any missing output fixed or listed.
4. `make -C runtime test test-services test-ipc-reference` green; four-title boots no regression.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared data read-only, locks, local-only, never ask).

## Result
