# Backlog (ranked; next rounds)

- nvn: `nvnTextureBuilderGetViewOffset` looked up by ACNH but not implemented (needs `gfx/src/nvnapi/names.txt` + definition) - reported by gfx-msaa-views r1
- input/IR camera (irs) - M, +0.05
- nvdrv nvhost-gpu/as-gpu/ctrl-gpu layouts vs real traces - needs Switch hardware (unreachable on Mac)
- media: AAC movie audio hookup (no title of the four creates a type-4 decoder) - media-live r1
- media/nvdrv: nvjpg/msenc/nvdisp on a title (none of the four opens them) - media-live r1

## After round 2 (from the 2026-11-01-ae99ac1 plan)
- svcGetInfo ids 19/27 (`kernel.c:440`) and kernel follow-ups — after sched-applets lands
- IPC depth for the applet (am) interfaces in `applet*.c` — after sched-applets lands
- translator `fail()` paths gfx-shader-views finds but cannot fix in round 2
- scan.py: read `runtime/tests/ipc_reference/` so IPC depth is measured, not estimated (catalog change)
- needs material, not reachable with the four titles: tess/XFB on a title, nvjpg/msenc/nvdisp/caps on a title, NCA0/NCA2 retail files; in-game GS draw needs a later-game SMO save

## Parked: paid plans (user decision 2026-11-02 — later, after Stripe + some users)
- Free: unlimited online play, rooms, friends, profiles; cloud saves for 1 title (5 versions, 256 MiB) + restore points for it.
- Napple Plus 2.99 USD/mo or 24.99/yr: unlimited E2E cloud saves (30-day history, 10 GiB), settings sync, album sync (5 GiB), 3 persistent private
  rooms, beta channel, cosmetic badge. Family 4.99/mo or 39.99/yr for 4 accounts. 14-day trial without card. Never gates online play; data never
  held hostage (read-only 90 days after downgrade). Stripe Checkout/Portal/webhooks; config-driven entitlements; launch with a grace period.

## iOS device (user report 2026-11-02, iPhone 16 Pro Max)
- SMO: 60 fps at 1x, then out of memory (jetsam) after ~1 min. Next round item: `com.apple.developer.kernel.increased-memory-limit` (+ extended
  virtual addressing) entitlements in the iOS build, memory footprint audit (guest heap mapping, texture/shader caches, RomFS map, staging buffers),
  Metal resource eviction under memory pressure (`didReceiveMemoryWarning`), purgeable caches; target: SMO 30 min on device without a crash.
- LAS: plays perfectly at 60 fps at 1x on device.

## Integration note for napple-social-cloud (r9)
- Apply the reserved-name rule (ACCOUNT-CONTRACT.md "Reserved names") to any user-visible free text it accepts (presence mode text, labels); it could not be messaged (cloud).

## Next UI item (after r8 lands): account-overlay-ui (macOS)
- New Account tab in the in-game overlay (`frontend/src/overlay.m`), per ACCOUNT-CONTRACT.md "Where the user sees it"; iOS panel later (iOS parked).
