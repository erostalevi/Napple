# online-friends (2026-11-02-r10): friend:u backed by napple-social

Branch `claude/online-friends-2026-11-02-r10`, Mac session. Integrator file: `docs/process/archive/integrators/claude-online-friends-2026-11-02-r10.md` (archived at
integration). Written by the integration session from the integrator file (the branch had no change note). Guide: [napple-online](../../guides/napple-online.md#friends-napple-social).

## What changed
- `runtime/src/net/net_online_social.c` (new): a napple-social client inside the napple-online wrapper. Access token from
  `napple_account_access_token()` (weak hook), `NAPPLE_ACCOUNT_TOKEN_FILE` or `NAPPLE_ACCOUNT_TOKEN`; profile from `NAPPLE_SOCIAL_PROFILE` or the
  account's first profile. Polls `GET /friends` (`NAPPLE_SOCIAL_POLL_MS`, default 5000), publishes presence "playing <title>" with a join token
  (`np1:<online server key>:<NEX pid>:<presence app data>`), heartbeats, goes offline at exit, raises friend-list / friend-request notifications.
- `net_online.c`: `ONLINE_CAP_FRIENDS` and the friend entries when a token source exists; reports known friend pids to napple-online
  (`POST /napple/v1/friends`). `online_provider.h` (append-only): `friend_list_info`, `friend_find`, `friend_request_count`, `friend_pop_note`,
  `friend_set_notify`, `friend_online_play`, `OnlineFriendInfo`.
- `offline.c` friend rows: FriendImpl 0x200 records (GetFriendList / Ids with filter, UpdateFriendInfo), counts, availability, GetUserPresenceView,
  the notification service, Declare* to the provider; `NAPPLE_FRIEND_LOG=1` traces every friend call. At integration these rows were merged with
  ipc-depth-95's friend_core rows: the provider answers when it serves the friend list, the cache rows otherwise.
- `crates/napple-online`: `/napple/v1/friends`, GetPlayingSession (31 / 60) from the caller's reported friends, Ranking 18 and MatchmakeExtension
  65 answer empty lists (MK8D's Friends screen treated NotImplemented as a communication error).

## Status
MK8D: each instance sees the other as online / joinable; the Friends / Create Room menu opens; the friend list on that menu stays empty (open).
Sending friend requests from the game, profile images, and a C unit test for the provider friend rows are not done.
