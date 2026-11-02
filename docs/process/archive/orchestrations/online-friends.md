---
name: online-friends
batch: 2026-11-02-r10
branch: claude/online-friends-2026-11-02-r10
base: claude/online-pia-2026-11-02-r7 + merge of claude/napple-social-cloud-2026-11-02-r9 (both land before this)
env: mac
model: claude-opus-5-5
depends_on: [online-pia, napple-social-cloud, napple-account]
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_b78f9b91-eea8-4eb8-9fed-7afbb16c9902 (fresh)
launched_at: 2026-11-02
---

# online-friends: the game's friend service (friend:u) shows Napple friends and presence; games invite through Napple

## Goal
`friend:u` already asks the active OnlineProvider (runtime/src/services/offline.c ~118). Implement `ONLINE_CAP_FRIENDS` in the Napple online glue
(`runtime/src/net/net_online*`) backed by the napple-social service (crate on claude/napple-social-cloud-2026-11-02-r9; API in its
docs/guides/napple-social.md; auth = napple-account JWT; social graph per Napple profile): friend list, friend profile info (nickname, icon),
presence (the runtime publishes "playing <title>" + a joinable hint when the game sets its presence / joins a session), friend requests the game can
send, invites/joins the game supports. Map Napple friend codes to the game-visible friend data. Offline / no server: unchanged behaviour.

## Owned paths
`runtime/src/net/net_online*`, the friend rows of `runtime/src/services/offline.c` (only where they must forward more to the provider),
`runtime/src/services/online_provider.h` (append-only), `crates/napple-social/` bug fixes, `crates/napple-online/` glue fixes, tests, change note,
`docs/guides/napple-social.md` (client section).

## Do not touch
gfx/, applet*.c (except nothing), fs.c, account*.c token paths (read only), napple-account crate, orchestrations/.

## Acceptance
1. Unit tests for the provider friends capability against a local napple-social + napple-account (or their test doubles).
2. **Verified on MK8D**: two instances with two Napple profiles that are friends (made via the social API): each sees the other in MK8D's friends list
   (online status / "playing MK8D"), and the Friends online mode lists the friend's room when hosting (if MK8D exposes it); screenshots/logs.
3. Without NAPPLE_ONLINE_SERVER everything is unchanged; runtime tests green; four-title boots unchanged.

## Env notes
Read orchestrations/ENV-MAC.md (shared data read-only, locks, local-only, never ask, never push). Two instances = one job under the run lock.
Integrated 2026-11-02 into main as 7036c84 (head d525548), outcome **Small fix** (friend-row merge with ipc-depth-95). Acceptance 2 partial, verified here:
friends see each other online / joinable in MK8D and the Friends menu opens, but its list stays empty. Acceptance 1 (unit test) not done.
Details: [integrator file](../integrators/claude-online-friends-2026-11-02-r10.md).
