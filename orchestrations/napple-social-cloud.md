---
name: napple-social-cloud
batch: 2026-11-02-r9
branch: claude/napple-social-cloud-2026-11-02-r9
base: origin/claude/napple-social-cloud-2026-11-02-r9 rebased by the orchestrator onto local main (21e785e); originally from publish 57431a7
env: linux-cloud
model: claude-opus-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: cloud (task_7708c053)
launched_at: 2026-11-02
---

# napple-social-cloud (cloud session; brief was given inline, see the integrator file on the branch)

## Acceptance (for the Mac integrator)
- `cargo test -p napple-social` on macOS, `cargo build --workspace --locked`; apply the reserved-name rule (ACCOUNT-CONTRACT.md) to any user-visible free text it accepts (presence mode text, labels): small fix or follow-up; verify JWT checks against the napple-account branch's JWKS shape.

## Result
