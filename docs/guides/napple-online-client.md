# Napple Online client core

The runtime-side client of Napple Online (orchestrations/ONLINE-CONTRACT.md): shared sign-in, account/profile/social calls, the local-user to profile
mapping, and the function table the overlay's Online tab uses. Code: `crates/napple-cloud/src/online/` (Rust) and `runtime/src/online/` (C).

## Shared credentials
`<NAPPLE_HOME>/online/credentials.json` (`NAPPLE_HOME` default `~/Library/Application Support/Napple`), mode 0600, read by every Napple game on the Mac:
`{server, account_id, device_id, refresh_token_enc, device_key_enc}` plus `account_key_enc` (the end-to-end save key once this device has it) and
`email`. The `*_enc` fields are AES-256-GCM (nonce || ciphertext, base64url, AAD `napple-online-credentials-v1`) under a key in the macOS Keychain
(service `lat.napple.online`, account `wrap-key`, any application of the user may read it, no prompt); with no Keychain (or `NAPPLE_ACCOUNT_STORE=file`)
a 0600 `wrap.key` next to the file holds it. Access tokens are memory-only.

Two games refreshing at once: every change is a read-modify-write under `flock(credentials.lock)` and lands by temp file + rename; the refresh HTTP
call happens inside the lock, so a rotated refresh token is never presented twice (the server ends all sessions of a device on reuse).
A 401 on refresh signs this Mac out (credentials cleared).

## Profile mapping
`<NAPPLE_HOME>/online/profile_map.json`: `{"default": "<profile_id>", "users": {"<uid hex>": "<profile_id>"}}`. The local Switch user takes its entry,
else `default`, else the account's first profile, which is recorded as `default` (no linking step). `switch_profile` writes the user's entry.
The friend service (net_online_social.c) gets the profile through `napple_account_profile_id()` unless `NAPPLE_SOCIAL_PROFILE` is set.

## C API
- Overlay table: `napple_online_runtime_start(home, account_url, social_url, uid_hex)` returns the `napple_online_ui_api` of
  `frontend/include/napple_online_ui.h` (hand it to `napple_frontend_set_online_ui`). One worker thread runs the actions and a state refresh
  (every `NAPPLE_ONLINE_POLL_MS`, default 30 s, and after every action). Completions and the change callback are queued: the host calls
  `napple_online_ui_pump()` on its main thread each frame. State getters read the cache.
- Game side: strong `napple_account_access_token()` / `napple_account_profile_id()` start the client lazily; when the Mac is signed in the
  friend service (ONLINE_CAP_FRIENDS) switches on by itself and shows the profile's Napple friends and presence. Signed out: nothing changes
  and no file or directory is created.
- Rust FFI (`napple_online_*`, JSON in/out): open, act, refresh, state, access_token, profile_id, free.

## Servers
`NAPPLE_ACCOUNT_URL` / `NAPPLE_SOCIAL_URL` (a trailing `/account/v1` or `/social/v1` is accepted), else `NAPPLE_ONLINE_SERVER`, else
`https://pretend.napple.lat`. Social calls carry `X-Napple-Profile`.

## Actions and endpoints
signup/verify/login/logout/reset_start/reset_finish -> `/auth/*`; switch_profile (local); create_profile `POST /profiles`; update_profile and set_tier
`PATCH /profiles/{id}`; upload_photo `POST /profiles/{id}/photo` (multipart field `photo`); friend_request `POST /friend-requests {friend_code}`;
accept/decline by the pending request found for that profile; remove `DELETE /friends/{id}`; block `PUT/DELETE /blocks/{id}`; search `GET /search?q=`;
invite `POST /invites`; join accepts the friend's live invite, else returns the join token of their presence; report `POST /reports`;
approve_device wraps the account key to the pending device (`/keys/requests`, `/keys/grant`); revoke_device `DELETE /devices/{id}`; delete_account `DELETE /me`.

## Tests
`cargo test -p napple-cloud online` (credentials, concurrent refresh, sign-in flow against a fake server) and
`make -C runtime test-online` (the table against a fake account+social server in C).

## Wiring (integrated, batch r12)
- `runtime/src/app.m` (windowed Mac runs): `register_online_ui()` calls `napple_online_runtime_start(NULL,NULL,NULL,NULL)`, hands the table to `napple_frontend_set_online_ui()` and pumps `napple_online_ui_pump()` from a 0.1 s main-run-loop timer. Without `CLOUD=1` the weak defaults in app.m return NULL and the overlay keeps its stub. Headless runs do not start the client until the friend service asks for a token (lazy start in `online_ui.c`).
- `frontend/src/overlay_online.m`: the shell registers the six social sub-tabs (`napple_online_social_init`, host toast = the game view toast, text input = the software keyboard bar awaited with a nested run loop) and keeps its own change callback on the table so `napple_online_social_changed` raises the friend-request toasts while the overlay is closed. Invite toasts (`napple_online_social_invite_event`) still need a feed from the client (no invite event in the table yet).
