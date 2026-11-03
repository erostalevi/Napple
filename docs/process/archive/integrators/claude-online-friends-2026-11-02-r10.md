---
branch: claude/online-friends-2026-11-02-r10
orchestration: online-friends
base: adf717a (online-pia) + merges of claude/napple-social-cloud-2026-11-02-r9 and claude/napple-account-2026-11-02-r8
head: see git log (commit after 8957222)
env: mac
date: 2026-11-02
areas: [runtime/net, runtime/services (friend rows only), crates/napple-online]
status: integrated
---

# claude/online-friends-2026-11-02-r10: friend:u backed by napple-social

## Summary
- `runtime/src/net/net_online_social.c` (new): napple-social client. Token from `napple_account_access_token()` (weak hook, for Studio/napple-cloud),
  `NAPPLE_ACCOUNT_TOKEN_FILE` or `NAPPLE_ACCOUNT_TOKEN`; profile from `NAPPLE_SOCIAL_PROFILE` or the account's first profile; polls GET /friends
  (`NAPPLE_SOCIAL_POLL_MS`, default 5000), publishes presence "playing <title>" (+ mode "online" while the title declares online play) with
  join token `np1:<online server key>:<NEX pid>:<base64url presence app data>`, heartbeats, DELETE at exit, friend-list / friend-request notifications.
- Friends on the same Napple online server are listed under their NEX pid (= the NSA id acc hands out), so game-server lookups work.
- `net_online.c`: ONLINE_CAP_FRIENDS + entries when a token source exists; reports known friend pids to napple-online (`POST /napple/v1/friends`).
- `online_provider.h` (append-only): friend_list_info, friend_find, friend_request_count, friend_pop_note, friend_set_notify, friend_online_play, OnlineFriendInfo.
- `offline.c` friend rows only: FriendImpl 0x200 records (GetFriendList/Ids with filter, UpdateFriendInfo), GetFriendCount with filter, newly count,
  request count, Check/EnsureFriendListAvailability, GetUserPresenceView, notification service (event + Pop {type, id}), Declare* -> provider,
  `NAPPLE_FRIEND_LOG=1` traces every friend call.
- `crates/napple-online`: `/napple/v1/friends` (in-memory per account), GetPlayingSession (31/60) with an empty list = caller's reported friends,
  Ranking 18 answers the empty RankingResult instead of NotImplemented.
- NOT done: MK8D's Friends mode still ends with "A communication error has occurred" (see Findings). Sending friend requests from the game
  (SendFriendRequestForApplication) not implemented: no NSA-id -> profile lookup exists. Profile images not served. Change note and guide client section not written. main not re-merged (usage limit).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| runtime builds | verified | `make -C runtime` (after `make -C input audio frontend gfx`, `make -C gfx adapter`) |
| existing tests | tested | test_svc_online 263/0 (record layout updated to 0x200), test_svc_lan_play 21/0, test_net_online_client 51/0, test_net_online_ssl 42/0 |
| napple-online tests | tested | `cargo test --release -p napple-online` 35 passed |
| Acceptance 1 (unit tests for the friends capability) | not done | no new C unit test; covered only by the e2e runs below |
| Acceptance 2 (two MK8D, friends) | partial, verified | real napple-account + napple-social + napple-online locally, two profiles made friends via the API; each MK8D lists the other (log `friend 4e41000000001001 'bob' now online play, same title, joinable`), bob's MK8D presence app data reaches alice (`GameMode 1, GID 1, EnableJoin 1`), alice's GetPlayingSession returns bob's gathering. MK8D Friends screen still shows the communication error. |
| Acceptance 3 (no server = unchanged) | proposed | friends only switch on inside the NAPPLE_ONLINE_SERVER wrapper with a token; four-title boots not rerun |

## Findings
- MK8D GetFriendList: in 64 bytes (offset, Uid at 8, filter at 0x18), out buffer 153600 = 300 x 0x200 -> FriendImpl is 0x200. UpdateUserPresence buffer 0xE0, app field at 0x20 (key\0value\0 pairs).
- MK8D Friends mode never puts friend ids in its NEX calls: Ranking 112/18 (19-byte body) and MatchmakeExtension 60 with an EMPTY pid list -> the server is expected to know the friends. Hence the friends report to napple-online.
- The remaining communication error happens ~0.5 s after GetPlayingSession returns bob's session; no friend:u call fails. Suspects: the PlayingSession reply structure (server writes {pid, gid, mode, attr0}), the Ranking 18 reply format, or a follow-up call. Next step: NAPPLE_TRACE_IPC=all + server -v after the fix, get the error code (prepo second buffer, see online-pia Needs #3).
- Friends reported to napple-online live in memory; after a server restart a client re-reports only when its friend pid set changes (or on relink).
- Evidence and scripts (friends_servers.sh, run_one.sh, run_pair.sh, logs, screenshot): side-session-notes/local_67fc7cb9-.../online-friends-r10/.

## How to integrate
- Merge after online-pia, napple-social-cloud, napple-account (already merged here). Cargo.toml members: kept online, room, social, account.
- Hotspots: offline.c friend rows (ipc-depth-95 converts other rows), online_provider.h tail, napple-online server.rs/http.rs/accounts.rs.
- `cargo build --release -p napple-online -p napple-social -p napple-account`, `make -C runtime`.

## How to test
1. Tests above.
2. `friends_servers.sh <dir> start`, then `lockf -k /Users/neto/NappleWork/.run.lock run_pair.sh <dir> <out> 75 "15:A,19:DOWN,20.5:DOWN,22:A,48:DOWN,49:DOWN,50.5:A" "15:A,19:DOWN,20.5:DOWN,22:A,32:A,36:A,40:A,44:A,48:A,53:A" 0.6`; pass today = logs above; full pass = alice's Friends screen lists bob's room.

## Needs from other areas
- infra: Caddy `handle /social/* { reverse_proxy social:8794 }`.
- napple-social / napple-account: an NSA-id (napple-online pid) -> profile link so SendFriendRequestForApplication can be resolved; ideally napple-online verifies friendships server-to-server instead of trusting the client's report.
- prepo: pass both buffers (error codes) — needed to diagnose the remaining MK8D error.

## Update (second pass, same day)
- main and claude/online-race-2026-11-02-r7 merged in (Cargo.toml members: all of room, online, account, cloud, social).
- **The communication error is fixed.** Cause (found by dumping the second prepo buffer, which carries the error report): MK8D's Friends screen
  calls Ranking 112/18 ("DownloadCompePopular") and MatchmakeExtension 109/65 ("GetCompeFavorite"), both NotImplemented in napple-online and fatal
  to the screen. Both now answer an empty list (server.rs). MK8D's Friends/Create Room menu now opens (screenshots pair4/alice/shot-3600.png in notes folder).
- **Still open:** the "Friends" list on that menu stays "-----" even though bob is online play / joinable in the friend list and GetPlayingSession (109/60)
  returns his gathering. Next: check what MK8D calls after 109/60 (NAPPLE_TRACE_IPC=all, server -v) and whether the friend's presence has to carry more
  than the 0xE0 app-field pairs, or whether the session reply needs the gathering fetched (MatchMaking get-session-by-id).
- Not done still: new C unit test, change note, guide client section.

## Final pass (gate)
- main merged again (docs-only change). Runtime service and net-online tests: all pass (svc_online 263/0 etc.); `cargo test --release -p napple-online -p napple-social` all green.
- Four-title boots without NAPPLE_ONLINE_SERVER, 45 s each, headless, private save roots: SMO 3ca12dfa (2659 frames), MK8D fe941ed5 (2429), LAS ae16f71e (2658): rc 0, no "napple social" lines, no fatal / not-implemented. ACNH ff1d1c05 (1280 frames): rc 0, no social lines, no fatal. All four boot unchanged.
- Blocking condition for the last acceptance item: MK8D's Friends menu opens and the friend is joinable in friend:u and GetPlayingSession (109/60) returns the room, but the menu list stays "-----". Not diagnosed further; next step is NAPPLE_TRACE_IPC=all + server -v on what MK8D calls after 109/60.

## Integration result
- Date 2026-11-02, merged into main as 7036c84 (head d525548), last item, after ipc-depth-95. Outcome: **Small fix** (merge resolution) plus doc commits.
- **Conflict with ipc-depth-95 in `offline.c` friend rows (semantic).** Resolved by the rule both sides stated ("online-friends' provider calls win,
  keep ipc-depth-95's other rows"): rows where this branch is IPC_IMPL take this branch's handler; rows this branch left as "unavailable" stubs keep
  ipc-depth-95's friend_core implementations. For the five rows both implemented (10102 UpdateFriendInfo, 10120 / 10121 availability, 20600 / 20601
  GetUserPresenceView) a small dispatcher answers from this branch's handler when the provider serves `friend_list_info`, else from ipc-depth-95's
  cache handler, so both branches' tests pass (test_svc_friend_r10 207/0; test_svc_online 263/0). This branch's `friend_presence_view` is renamed
  `friend_presence_view_provider`; its now-unused FRIEND_UNAVAILABLE stubs were dropped (they warned). **A human may want to review** that, with a
  friends provider, rows such as GetFriendDetailedInfo / GetProfileList now answer from ipc-depth-95's friend_core cache instead of "unavailable".
- Docs (missing on the branch): change note `docs/archive/changes/2026-11-02-online-friends-2026-11-02-r10.md`, a "Friends (napple-social)"
  section in `docs/guides/napple-online.md` (env variables, status), r10 change-archive row.
- `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0 (missing 0); test-net-online 51/0, 42/0; `cargo test -p napple-online
  -p napple-social --locked` green; `cargo build --workspace --locked` ok. No new warnings.
- **MK8D e2e on the merged tree** (the author's friends_servers.sh / run_pair.sh, adapted: local napple-account + napple-social + napple-online, alice
  and bob made friends via the social API, one job under the run lock, 75 s): each instance sees the other (`friend … 'bob' now online play, same
  title, joinable` / `'alice' online`), no error applet; alice's Friends / Create Room menu opens; its Friends list shows "-----" (open, as handed off).
- Four-title 45 s boots without a server, offline and `NAPPLE_NET=host`: all rc 0 and `missing_ipc []` except **one ACNH offline run that crashed with
  SIGBUS** (rc 138, fault 0x7014659a00 in the guest stack range, pc sdk+0x508334, ~3 s, right after `olsc:u` opened); two reruns of the same binary
  were rc 0, and about 20 other ACNH boots today were clean. Intermittent; listed for follow-up (olsc rows changed in ipc-depth-95).
- Not done (as declared): a C unit test for the provider friend rows (not mechanical: needs a fake friends provider), friend requests from a game,
  profile images.
