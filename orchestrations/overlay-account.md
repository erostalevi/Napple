---
name: overlay-account
batch: 2026-11-03-r12
branch: claude/overlay-account-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# overlay-account: overlay Online tab: shell, account screens, profile editor, tier picker, devices; defines the UI API header

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
new `frontend/include/napple_online_ui.h` (the function-table header from the contract), new `frontend/src/overlay_online*.m` (tab shell + account/profile screens + a canned-data stub implementation), small appended hooks in `frontend/src/overlay.m` and `settings.c`, tests/screenshots under `frontend/tests/` or `tools/golden/`, `docs/architecture/audio-input-frontend.md` (append), change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Write `napple_online_ui.h` first (exactly the contract's API; commit it early so client-core and overlay-social can use it) and a stub implementation with canned data so every screen works without servers.
2. Add the **Online** tab to the settings overlay (macOS; keep the existing four tabs untouched) with: signed-out screens (create account: email, password, nickname, 13+ and terms checkboxes with links to the legal pages; enter the 6-digit code; sign in; forgot password/reset), signed-in header (profile photo as a circle, nickname, tier), profile editor (nickname, bio, up to 3 links, built-in icon grid, photo picker via NSOpenPanel + preview, tier picker with the one-line descriptions: Public, Friends (searchable), Friends (hidden), Private, Ghost), profile switcher/creator (up to 8), devices (approve/revoke), cloud-save status per title, account deletion with confirmation.
3. Controller navigation (focus ring, A/B, d-pad) and the on-screen keyboard via the existing software keyboard applet for text fields; errors and progress states for every async action.
4. The tab shell must expose a way for overlay-social to add the Friends/Stats/Invites sub-tabs (separate files `overlay_online_social*.m` owned by that item).
5. Verify headless: render each screen to PNG (screenshot hook) with the stub; `make -C frontend`/gfx tests green. Do not break the existing overlay tabs.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
