---
branch: claude/social-tiers-2026-11-03-r12
orchestration: social-tiers
base: ac6692c
head: pending
env: mac
date: 2026-11-03
areas: [crates/napple-social, docs/guides]
status: ready-with-caveats
---

# claude/social-tiers-2026-11-03-r12: napple-social visibility tiers, search, request rules, lobbies, recent players, reports, stats

## Summary
Implemented the Social section of the Online contract in crates/napple-social: the five tiers as pure rules (src/visibility.rs), tier-aware search,
friend requests by code or profile_id with the shared-lobby exception, filtered public profile view, stats (user + service), lobby views with random
ghost aliases, recent players (/encounters, 30 days, 100 per profile), reports (incl. ghosts by alias), blocks hiding both ways, per-endpoint rate limits,
and a tier/photo snapshot from napple-account with a service-credential directory lookup (TTL cache, failure keeps last snapshot). Not done: nothing from the
assignment; the swap to crates/napple-visibility is pending (that crate did not exist yet).

## Files
crates/napple-social/{src/visibility.rs,src/social.rs,tests/tiers.rs} new; src/{api,auth,config,db,lib}.rs and tests/common/mod.rs changed;
docs/guides/napple-social.md (new section); docs/archive/changes/2026-11-03-social-tiers.md. No shared files touched (no Cargo.toml/Cargo.lock change).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Tier x action cells (search, request, card, presence, stats, lobby, recent, ghost) | tested | cargo test -j2 -p napple-social: 22 unit + 59 integration pass, 30 in tests/tiers.rs |
| Blocks both ways, rate limits (search, reports), service credential | tested | same |
| healthz kept, env names | tested | tests/auth.rs healthz; docs env table |
| Against a real napple-account | untested | mock account service only |

## Unbuilt / untested list
- Directory lookup `GET /account/v1/internal/profiles/{id}` with X-Napple-Service: no real server implements it yet (proposed).
- Schema migration of an existing v1 database (ALTERs): covered only by the fresh-DB path; logic is idempotent column checks.

## Findings / decisions
- Tier source: owner's ownership list (60 s cache) is stale-prone, so the directory lookup (service token, TTL NAPPLE_SOCIAL_PROFILE_TTL_SECS=10) is authoritative; a list snapshot only overwrites a row if it is newer (a stale list can never un-ghost someone).
- Hidden = 404, never 403. Friends of a ghost keep the list entry but see offline and no events.
- Shared-lobby proof: napple-online registers lobbies via POST /internal/lobbies (service token); an /encounters match also counts for 24 h. Friend codes cannot find tier 3-5 unless the proof holds for tier 3.
- Lobby aliases Bot-XXXX are generated here and returned to the registrar so the game name matches the alias used for reports.
- tier module is a private copy named like crates/napple-visibility (Tier, Kind, can_view); swap = replace src/visibility.rs by a re-export.
- Reports keep both real account ids and survive profile deletion (no FK); no retention job yet.

## How to integrate
- After account-profiles: replace src/visibility.rs body with `pub use napple_visibility::*` if signatures match (Viewer struct may differ; adapt).
- Build: `cargo build -j2 -p napple-social`; test: `cargo test -j2 -p napple-social`.
- Conflict hotspots: none outside crates/napple-social and docs/guides/napple-social.md.

## How to test
1. `cargo test -j2 -p napple-social` all green.
2. Run server with NAPPLE_SOCIAL_SERVICE_TOKEN=x NAPPLE_ACCOUNT_SERVICE_TOKEN=y; call POST /social/v1/encounters with X-Napple-Service: x.

## Fix policy hints
Safe: path/field-name alignment with the real account service. Design decision: the directory endpoint shape.

## Needs from other areas
- napple-account: `GET /account/v1/internal/profiles/{id}` (header X-Napple-Service = NAPPLE_ACCOUNT_SERVICE_TOKEN) returning the profile JSON incl. `tier` and `photo`; also `tier`/`photo` in `GET /profiles` items.
- napple-online: call POST /internal/lobbies when a room forms, POST /encounters after matches, POST /internal/stats; use returned aliases for ghosts; same NAPPLE_SOCIAL_SERVICE_TOKEN.
- infra: set the two service tokens as secrets.
