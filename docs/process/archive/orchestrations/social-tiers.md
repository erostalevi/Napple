---
name: social-tiers
batch: 2026-11-03-r12
branch: claude/social-tiers-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_b7088465-f863-4480-9dbd-bd47ff08ac82
launched_at: 2026-11-03
---

# social-tiers: napple-social: five visibility tiers, search, request rules, recent players, block/report, per-title presence and stats

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
`crates/napple-social/` (whole), `docs/guides/napple-social.md`, change note. Use the shared tier rules module `crates/napple-visibility` (created by account-profiles; until it exists, implement the table in a private module with the same function names and swap later).

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Implement the Social section of the contract and the tier table: search (nickname prefix, friend code) respecting tiers, request rules incl. shared-lobby exception (a `shared_lobby` proof the napple-online server vouches for via a signed short-lived token or an internal endpoint), block hides both ways, recent-players (`/encounters` called by napple-online with the service credential), reports, per-title presence with the reserved-name rule on free text, stats read/write (tier-filtered).
2. Profile data (nickname, tier, photo ref) comes from napple-account `GET /profiles` with the caller's token or a service token: cache with a short TTL; mockable in tests.
3. Tests for every tier x action cell of the table (search, request, presence, stats, lobby view, recent) including ghost; blocks; rate limits.
4. `cargo test -p napple-social` green; `GET /social/v1/healthz` kept; env names from the contract.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result

Outcome: Small fixes (contract alignment with napple-account / napple-visibility). Merged at 19968c7. Details: [integrator result](../integrators/claude-social-tiers-2026-11-03-r12.md).
