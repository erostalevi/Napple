# Round 11 (2026-11-03): five focus domains to 100% — Sonnet 5.5, medium effort

Mac (local sessions, claude-sonnet-5-5 medium): ipc-risk-mac-checks, nvn-api-tail, nvn-viewport-swizzles, gfx-xfb-tess-audit, media-synthetic-streams (files in this folder).
Cloud (briefs inline; branches claude/<name>-2026-11-03-r11, rebased onto local main at integration; cloud sessions use the environment's default model, effort not settable):
ipc-domain-reuse, ipc-bcat-npns-local, ipc-fs-mii-ldn-tail, ipc-provider-hooks, nvdrv-gpu-routing, nvjpg-headerless.
After integration (/napple-integrate): back to MK8D online play (race joiner 2618-0502, Friends list, Wireless Play) + socials/friends + profile and create-account web pages on pretend.napple.lat (website, not servers).

## Cloud outcome (user's credits ran out, 2026-11-03)
Finished and pushed by cloud (kept as local branches exactly as pushed, base = published squash d0d4be9/050f4dd, NOT rebased: rebase conflicts with round-10 files):
- ipc-domain-reuse (conflict: docs/archive/changes/README.md — append-only, keep both), ipc-bcat-npns-local (conflict: runtime/src/services/bcat.c vs ipc-depth-95/-cloud), nvdrv-gpu-routing (conflicts: nvdrv_nodes_core.[ch], test_svc_nvdrv_nodes.c vs nvdrv-public-layouts), nvjpg-headerless (conflict: jpeg_core.h). The Mac integrator merges them with `git merge` and resolves the conflicts semantically (keep both behaviours; round-10 code is on main), builds on macOS, runs the suites.
Restarted locally (cloud had pushed nothing): ipc-fs-mii-ldn-tail, ipc-provider-hooks (sessions local_f85e99a7..., local_3cb21158...).
