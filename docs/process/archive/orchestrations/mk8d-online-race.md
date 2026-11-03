---
name: mk8d-online-race
batch: 2026-11-03-r12
branch: claude/mk8d-online-race-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_758b1292-20b6-4197-8b16-da4452d4fb81
launched_at: 2026-11-03
---

# mk8d-online-race: MK8D full online: race joiner, pair flakiness, Friends list, Wireless Play

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
`crates/napple-online/src/` NEX/PRUDP/NAT files (not the identity/ghost/matchlog/reporting files another session adds), `runtime/src/services/{bsd*,offline.c (nifm, ssl, friend blocks),ldn*}`, `runtime/src/net/` (bug fixes), tests, `docs/guides/napple-online.md` (MK8D section), `compat/online.json` (update statuses), change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
Current state (see docs/process/archive/integrators/claude-online-race-*.md, claude-online-friends-*.md, claude-mk8d-lan-session-*.md, claude-online-pia-*.md): two MK8D instances log in, reach the online menu, match into one gathering and pass the NAT probe; the joiner then ends with 2618-0502 (~150 ms after the probe, no connection request sent); about half of the pair runs instead end at ~50 s with 2618-0201 and no NAT traversal; the Friends mode list is empty though the friend shows joinable; Wireless Play (LDN) station crashes on a null vtable after joining.
1. Next step recorded by the previous session: watch the Pia connect job's state field `[job+0x40]` with NAPPLE_WATCH and find which ConnectionFailed call site fires (0x99df54, 0x99e294, 0x99e420, 0x99e580, 0x932aa8, 0x93285c, 0x932f60 in main) and the compare feeding it; fix generically.
2. Fix the 2618-0201 timing flake (run pairs 10 times; both outcomes happened on the same binary).
3. Friends list: find which method the Friends screen calls after the empty list (NAPPLE_TRACE_IPC=all) and make it list friends (the client-core item wires the provider hooks; you own the service rows).
4. Wireless Play station crash: diagnose and fix.
5. Goal: two MK8D instances reach the race lobby and race through a local napple-online (+ room); if still blocked, hand off with the exact blocking condition and evidence. Update compat/online.json truthfully.
Pairs run as ONE job under the run lock (<=120 s).

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result

Outcome: Clean (doc conflicts with the repository cleanup resolved toward main). Merged at c186b8b. Joiner 2618-0502 fixed, MatchmakeExtension 49, LDN Scan pacing; a two-instance pair reaches the race countdown (session-verified; not re-run at integration). Open: Wireless Play joiner crash (main+0x9a3c84), Friends list empty, 2618-0201 flake not studied, race not driven to the end. Details: [integrator result](../integrators/claude-mk8d-online-race-2026-11-03-r12.md).
