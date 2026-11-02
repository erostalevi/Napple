---
name: online-identity
batch: 2026-11-03-r12
branch: claude/online-identity-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_1c5d6eda-4aa5-433b-bacf-41b0c083ab39
launched_at: 2026-11-03
---

# online-identity: napple-online: accept account JWTs, ghost alias, match log, encounters/stats reporting

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
new files in `crates/napple-online/src/` (`identity.rs`, `ghost.rs`, `matchlog.rs`, `reporting.rs`) plus small hooks in the existing request/session files; tests in `crates/napple-online/tests/`; docs `docs/guides/napple-online.md`, change note. Another session (mk8d-online-race) edits the NEX/PRUDP/NAT files: do not touch those.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Replace napple-online's own account/token issuing with verification of napple-account access JWTs (Ed25519 via JWKS with cache); the game-visible principal derives deterministically from `profile_id` (document the mapping).
2. Ghost (tier 5): random bot-style alias per session, never the real nickname/photo, stable within a session, collision-free among players in a match.
3. Match log: for every match record real account, profile, title, match id, participants, start/end; 30-day retention with a purge job; export for reports (`POST /online/reports` resolves a reported alias/match to the real account for an admin).
4. After a match report encounters and per-title stats to napple-social with the service credential; shared-lobby proof for friend requests.
5. Tests with mock JWKS/social; `cargo test -p napple-online` green. Keep the existing wire behaviour for MK8D unchanged (the login/online menu path still works).

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result

Outcome: Small fixes + mechanical conflict (changes index). Merged at a089f3f. Details: [integrator result](../integrators/claude-online-identity-2026-11-03-r12.md).
