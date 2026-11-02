---
name: legal-pages
batch: 2026-11-03-r12
branch: claude/legal-pages-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_18b9540d-679c-457c-888c-59bbb9416a32
launched_at: 2026-11-03
---

# legal-pages: terms, privacy policy, age rule, report policy (static pages on napple.lat + docs)

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
`site/terms.html`, `site/privacy.html`, `site/style.css` (append), `site/index.html` (footer links only), `docs/legal/` (new: the same texts as markdown, `docs/index.md` link), change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
Write careful plain-English documents for the Napple Online service (not legal advice; keep them short and readable) in the landing page's style: Terms (acceptable use; accounts and suspension; no warranty; Napple is independent and unaffiliated with Nintendo; users must own their games; user content licence for profile text/photos; 13+ age rule; termination), Privacy (exactly what is collected: email, password hash, nickname, bio, links, photo (re-encoded copy only), friends, presence and per-game stats, devices, encrypted cloud saves we cannot read, the 30-day match log with real-account mapping even for Private/Ghost tiers, IP in transient logs; processors: AWS (hosting, SES email); no ads, no tracking, no data sales; retention; user rights: access/export and deletion from the in-game Online tab; EU/UK GDPR basics, contact address `legal@napple.lat` placeholder noted), Report/takedown policy. Version both documents (v1, date) because signup stores the accepted terms version. Add footer links on the landing page; checks: `python3 tools/check_docs.py`, links valid.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
Outcome: Small fix (clean merge). Merged at d3329de. Details: [integrator result](../integrators/claude-legal-pages-2026-11-03-r12.md).
