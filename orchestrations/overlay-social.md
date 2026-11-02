---
name: overlay-social
batch: 2026-11-03-r12
branch: claude/overlay-social-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: [overlay-account]
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# overlay-social: overlay Online tab: friends, requests, add/search, block, join/invite, per-game stats, toasts

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
new `frontend/src/overlay_online_social*.m` and tests, small appended hooks in the tab shell file only where overlay-account exposes extension points, change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
Start when overlay-account's shell and `napple_online_ui.h` exist on its branch/main (merge them in). Implement against the header + stub: friends list with presence (online / playing <title> / offline, joinable badge), pending requests (accept/decline), add by friend code or nickname search (tier-aware results from the server), remove, block list, recent players (with report and add), join and invite actions (invite accepted/received as toasts over the game), per-game stats list for the current profile (all titles), toasts for friend requests/invites arriving. Controller navigation and on-screen keyboard like the other screens. Render each screen to PNG with the stub; no regression in the other tabs.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
