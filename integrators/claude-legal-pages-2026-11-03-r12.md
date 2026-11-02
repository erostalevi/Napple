---
branch: claude/legal-pages-2026-11-03-r12
orchestration: legal-pages
base: ac6692cb300be6fc6e90eb894de0268c3732e948
head: f5e81ef
env: mac
date: 2026-11-03
areas: [site, docs/legal]
status: ready
---

# legal-pages: terms v1, privacy v1, report policy v1

## Summary
Plain-English Terms (13+ age rule, own your games, not affiliated with Nintendo, content licence, suspension, no warranty), Privacy (exact data list, 30-day match log with real-account mapping even for Private/Ghost, AWS/SES processors, no ads/tracking/sales, retention, GDPR rights via Online tab, legal@napple.lat placeholder) and Report/takedown policy. Static pages in landing style plus markdown copies. Signup should store terms version string `v1`.

## Files
docs/legal/{README,terms,privacy,report-policy}.md, docs/legal/gen_pages.py (regenerates site HTML from markdown), site/{terms,privacy,report}.html (report.html added beyond the two named pages), site/style.css (appended), site/index.html (footer links only), docs/index.md (one appended line), change note.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Docs check | verified | `python3 tools/check_docs.py`: no problems in new files; 20 pre-existing orphans in docs/process/archive |
| Pages render/links | tested | generated HTML, relative links only; not viewed in a browser |

## Unbuilt / untested list
None (static content).

## Findings
Decisions: retention numbers not in the contract (IP logs <=14 d, backups ~35 d, reports 12 months), US-law-neutral governing law clause, report.html added. Owner must confirm legal@napple.lat and ideally get a lawyer review. Privacy text asserts export/deletion in the Online tab (contract lists delete_account; an export action is not in the overlay API, so the data export promise needs implementing or a softer wording).

## How to integrate
No ordering constraint. Hotspots: docs/index.md (append-only), site/index.html footer. Edit markdown then run `python3 docs/legal/gen_pages.py`.

## How to test
1. `python3 tools/check_docs.py` (no legal/ problems).
2. Open site/terms.html, privacy.html, report.html and the index footer links.

## Fix policy hints
Wording changes are safe; retention numbers must match the real server.

## Needs from other areas
Signup (napple-account) store terms version `v1`; overlay/account need an export-my-data action to honour the Privacy text.
