---
name: online-tieup
batch: 2026-11-03-r12
branch: claude/online-tieup-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-opus-5-5
depends_on: [account-auth-ses, account-profiles, social-tiers, online-identity, client-core, overlay-account, overlay-social, mk8d-online-race, infra-online, legal-pages]
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# online-tieup: end-to-end online verification and glue

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
new `tools/e2e/online/` (scripted end-to-end), small glue fixes anywhere ONLY where verification finds integration bugs (one commit each, message `tieup:`), `docs/guides/online-e2e.md`, update `infra/local/run.sh` usage docs, change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
Start after the other r12 items are integrated on main. Implement and run the Definition of done in the contract as one script: start local account+social+online(+room) (dev mailer), create an account through the client core (headless call of the overlay API or CLI), read the code from the dev mail log, verify, log in on a second NAPPLE_HOME (second device) incl. the E2E key approval, make a save on device A and see it pulled on device B before mount, create a second account and add each other as friends by code, exercise every tier cell for search/presence, then two MK8D instances through napple-online (report how far). Fix small integration bugs; list bigger ones. Also run the overlay screens once with the real client against the local servers and capture PNGs. Output a pass/fail table with evidence. The final report names anything not verified (real SES mail, real AWS).

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
