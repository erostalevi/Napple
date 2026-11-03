# Integrator: ipc-provider-hooks (2026-11-03-r11)

Branch: `claude/ipc-provider-hooks-2026-11-03-r11` (local only, not pushed). Change note: `docs/archive/changes/2026-11-03-ipc-provider-hooks-2026-11-03-r11.md`.

## Rows converted (12 `IPC_STUB` to `IPC_IMPL`)
**Phase 1 (friends / nifm)**
- friend:u: 10110, 10111 (profile picture), 10200 (friend request), 11000, 11001 (picture URL). New provider hooks `friend_profile_image`,
  `friend_send_request`, `friend_image_url`; the NullProvider keeps the offline "network service account unavailable" answer.
- nifm IRequest 22 GetAdditionalInfo and 26 GetNetworkAccessStatus (the brief called them IGeneralService; they are IRequest rows) from `net_info`, so
  NAPPLE_NET=host answers with the Mac's interface once a request is accepted.

**Phase 2 (account / cloud)**
- acc:u0 102 AuthenticateApplicationAsync, 120 CreateGuestLoginRequest and IManagerForApplication 150 CreateAuthorizationRequest via the existing
  `account_id_token` (the Napple account token path of `net_online`). 120 / 150 hand out `IGuestLoginRequest` / `IAuthorizationRequest` objects.
  `IAuthorizationRequest::GetAuthorizationCode` is a new `IPC_STUB` row (an OAuth code needs a Nintendo Account server).
- olsc:u 18, 27 via new hooks `cloud_backup_info` / `cloud_backup_upload`.

## Decisions (the user was away)
- Phase 2 needed no new account hook: `account_id_token` already exists. 102 / 120 / 150 answer success only when the provider issues a token for the local user.
- 11000 / 11001 are modelled as "rewrite the input URL for an edge size" (the shape switchbrew gives), so `friend_image_url` takes the URL, not an id.
- No `net_profile_name` hook: the nifm profile is already built from `net_info`.
- Cloud-save state has no C-readable per-user status in `runtime/src/cloud` (read-only for this task), so the Napple glue does not implement
  `cloud_backup_*`; the hooks and the services are ready for it. Same for the three friend hooks in `net_online*`: the social server has no image / URL / by-id
  request endpoint. Without them, the Napple online provider answers these rows like an offline console (unchanged from before).
- `online_provider.h` changes are append-only at the end of the table; `ONLINE_ABI_VERSION` unchanged. `struct OnlineCloudBackup` is defined after the table.

## Files
`runtime/src/services/{online_provider.h, offline.c (friend + nifm blocks), account.c, olsc.c}`; tests `runtime/tests/test_svc_provider_hooks{,_acc,_olsc}.c`;
`test_svc_nifm_audit.c` and `test_svc_fsacc_r10_olsc.c` (two assertions flipped from STUB to IMPL). No change to ipc.c, bcat/push, fs.c, mii_img.c, ldn*, gfx/, crates/.

## Tests
`make -C runtime GFX=napple test test-services test-ipc-reference test-net-host`: rc 0, 87 JSON lines with `failed:0` (needs `rustc` on PATH for the
napple-room build in test-net-host). New tests: 50 + 60 + 36 checks. Four-title boots: see the Boots section.

## Risks
- Every layout beyond the row names is `[unverified]` (marked in the code): the input offsets of 10110 / 10111 / 10200 / 11000, the 0x10 record of nifm 22, the
  value of 26, the 0x20 record of olsc 18, the IAuthorizationRequest ids (0, 10, 19, 20, 21 per switchbrew memory).
- A real title that calls 120 / 150 now gets objects when NAPPLE_ONLINE_SERVER is set and the account is linked; if a title uses other ids on them it
  falls to the unknown-command policy (loud, not silent).
- `docs/architecture/runtime-services.md` lists of remaining STUB rows were not edited (outside ownership); the change note carries the list.

## Boots
`runtime/build/napple-runtime <title>/<build> --headless --seconds 60`, private `--save-root`, under `lockf -k /Users/neto/NappleWork/.run.lock`, each of
0100000000010000, 0100152000022000, 01006BB00C6F0000, 01006F8002326000 without and with `NAPPLE_NET=host`: all 8 runs rc 0, summary status `partial`
(the 60 s limit), `missing_ipc []`, `unsupported_svcs []`, `crashes []`, no CRASH line in any log.

## Integration result
- Date: 2026-11-03 (batch r11 integrator run, local main, unpushed)
- Merged commit: 96841c4
- Outcome: Clean (landed by cherry-pick)
- Same published-squash base problem as ipc-fs-mii-ldn-tail: its five commits (8606abd 09de5da 3cac915 35ae81f e9bb417) were cherry-picked onto main without conflicts (no offline.c/account.c/olsc.c clash with the ipc-fs-mii-ldn-tail landing). gfx 0 skipped, runtime test/test-services/test-ipc-reference green (missing 0), test-risk-title 158/0. LAS corpus 5767/5767/5767 was run after the gfx landing; boots at the end.
