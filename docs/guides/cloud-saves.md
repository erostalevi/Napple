# Cloud saves (Napple Account, end-to-end encrypted)

One Napple Account on the Mac and the iPhone: a game continues from the same save on both. Saves are encrypted on the device before upload;
the server (`crates/napple-account`, contract in `orchestrations/ACCOUNT-CONTRACT.md`) stores ciphertext only and never sees a key.
Nothing syncs unless you sign in. The client is `crates/napple-cloud` (Rust), used by the `napple` CLI and linked into the native runtime
(`runtime/src/cloud/`).

## Using it

```bash
napple account login                 # email + 6-digit code; this Mac becomes a device of the account
napple account status
napple saves list                    # cloud + local saves, sync status, the plan's synced title
```

- **First device** of an account: makes the account's save key and prints a **24-word recovery code** once. Write it down: it is the only
  way back in if every device is lost (the server cannot help: it never has the key).
- **Another device** (an iPhone, a second Mac): after `login` it waits for the key. On a device that is already signed in run
  `napple account approve` (or `napple account devices` to see who is waiting), or on the new device `napple account recover`.
- **Playing**: launch the game as usual. Before the game opens its save, the runtime downloads the newest cloud version (bounded by 8 s; offline
  or slow = the game starts from the local save). After the game commits its save the runtime uploads it in the background (5 s after the
  last commit, never on the game thread), and again on a clean exit (at most 10 s).
- **Conflicts** (both devices played from the same version): nothing is lost. The cloud version becomes current, this device's save is
  kept as a **restore point** (`napple saves history <title>`), and the frontend is told (event `conflict`). `napple saves restore <title> <file>`
  brings it back (it then becomes the newest cloud version).
- **History**: the server keeps the last versions (Free 10, Plus 20). `napple saves restore <title> <version>` makes an old one current.
- **Per title**: `napple saves disable <title-id>` / `enable` (default on after sign-in).
- **Offline**: an upload that fails for a network reason is queued (`queue.json`) and retried with backoff while the game runs, at the next
  launch, or with `napple saves push`. A local save is never deleted or overwritten unless the same data is in the cloud or in a restore point.
- **Sign out**: `napple account logout` removes the tokens and keys; local saves and restore points stay.

### Plans

Free: account, devices, profiles, online identity, and cloud saves for **one title** (the first one you sync; switchable once every
30 days, server side). **Napple Plus** (subscription): every title, 20 versions per title, 5 GiB. A save upload beyond the plan gets
`402 plan_limit`: the save stays on the device, the title shows **not synced (plan limit)** in `napple saves list` and the runtime log/event,
and `napple account upgrade` opens the checkout page (`plan.upgrade_url`). Downloads and restores of existing versions always work.
Apps never take payment themselves.

### Local users and profiles

A Napple Account has up to 8 player profiles. Cloud saves are per (title, profile). Each local user of the runtime (acc:u0 uid; the first one
is `4e617070706c65557365720000000001`) is linked to a profile: `login` links the first user to the account's first profile (making one if
there is none); `napple account link <profile-id> --user N` changes it. Use the same profile on the Mac and the iPhone to share a save.

## What is synced

`<save root>/<TITLE>/save` (runtime `host_save_dir`), for each linked user, as one version:
- `account/`: the user's account save, `space1/account/<first 4 uid bytes>` (the **committed** snapshot `<dir>.committed`: what a crash
  would roll back to; uncommitted writes are not uploaded),
- `device/`: the title's device save `space1/device` when it exists (some titles keep their data there).
Installing a version replaces both the snapshot and the live directory of each component it contains, before the game opens them.
Caches, BCAT, temporary and system saves are not synced. Nothing else ever leaves the device (no game content, keys or firmware).

## Security model

- **Device key**: X25519, generated at sign-in. The private key stays in the Keychain (`lat.napple.account.<namespace>`, account
  `device-key`), never synchronized. The Secure Enclave only holds P-256 keys, so it cannot hold this one.
- **Account save key**: 32 random bytes made by the first device. Each device gets it wrapped to its public key with **HPKE base mode**
  (RFC 9180, DHKEM(X25519, HKDF-SHA256) + HKDF-SHA256 + AES-256-GCM, `info = "napple-account-save-key-v1"`, empty AAD). Approving a device =
  an existing device wraps the key for it (`POST /keys/grant`). The server stores only wrapped keys.
- **Recovery**: 24 BIP-39 words (256-bit entropy). KEK = Argon2id(entropy, 16-byte salt, 64 MiB, t=3, p=1); the record
  `{kdf:"argon2id", m_kib, t, p, salt, wrapped}` (wrapped = nonce(12) ‖ AES-256-GCM(KEK, account key, AAD `napple-recovery-v1`)) is stored
  with `POST /keys/recovery`.
- **Per-version key**: HKDF-SHA256(ikm = account key, no salt, info = `napple-save|<TITLE>|<profile_id>|<version>`), so a blob only opens
  as the title, profile and version it was made for (the server cannot swap versions or titles).
- **Tokens**: refresh token (rotated at every refresh) and the cached access token in the Keychain. macOS: the login keychain with an
  any-application decrypt ACL so the CLI, Studio and every built game (different binaries, one device) read them without a prompt; the
  keychain never shows UI (a locked keychain is an error). iOS: `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`.
  Follow-up once Studio and the games are signed with one team ID: the data-protection keychain with a shared access group on macOS too.

## Wire details (what the contract leaves open; the client implements these, the stub in `crates/napple-cloud/src/stub.rs` too)

- Base URL `https://pretend.napple.lat/account/v1` (`NAPPLE_ACCOUNT_URL` overrides). JSON field names as in the contract (snake_case).
- Binary fields (`device.pubkey`, `wrapped_account_key`, recovery `salt`/`wrapped`) are **base64url without padding** (padded or standard
  base64 is accepted when reading).
- `POST /auth/email/finish` (and `/auth/refresh`) answer `{access_token, refresh_token, expires_in, account_id, device_id}` (a nested
  `tokens` object is accepted too). Refresh tokens rotate: the answer's refresh token replaces the old one.
- `device.platform`: `macos` for the Mac (CLI, Studio and games share the device), `ios`, `cli` elsewhere.
- `GET /keys/mine` -> `{wrapped_account_key}`, **404** while the device has no key. `GET /keys/requests` -> `[{device_id, name, platform,
  pubkey, created_at}]`. `GET /keys/recovery` -> the stored record (**404** if none) — the contract only names the POST; the client needs
  the GET for `napple account recover`, and uses "no other device and no recovery record" to decide it is the account's first device.
- Lists may be a bare array or an object holding it (`devices`, `profiles`, `requests`, `versions`, `saves`).
- `GET /saves/{title}/{profile}/versions` -> `[{version, size, sha256_ciphertext, created_at, device_id}]` (committed versions; an entry with
  `committed: false` is ignored). Title ids are 16 upper-case hex digits.
- New version: `POST .../versions {base_version, size, sha256_ciphertext}` -> `{version, upload_url}` where **version must be
  `base_version + 1`** (the first version is 1, `base_version` 0): the client derives the blob key from the version before it asks, because
  `sha256_ciphertext` is part of the request. `409 {error, head}` (`head` a number, or `{version}`) when `base_version` is not the head.
  `402 {reason: "plan_limit", upgrade_url}`. Upload: `PUT upload_url` with the blob (`application/octet-stream`), then
  `POST .../versions/{v}/commit {}`. Download: `GET .../versions/{v}` -> `{url}` (presigned GET).
- `GET /me` -> `{account_id, email, display_name, plan: {name: "free"|"plus", status, renews_at, synced_titles_limit (null = unlimited),
  quota, upgrade_url, synced_titles: [title ids]}}`. `upgrade_url` falls back to `<base>/billing/checkout`.

### Save blob, format 1

```
0   4   "NSAV"
4   1   format = 1
5   1   aead = 1 (AES-256-GCM)
6   1   compression = 1 (zstd)
7   1   reserved = 0
8   12  nonce = HMAC-SHA256(version key, "napple-save-nonce" ‖ compressed)[0..12]
20  ..  AES-256-GCM(version key, nonce, zstd(tar), AAD = bytes 0..20) ‖ tag
```
The tar is deterministic (ustar; entries sorted by path; mode 0755/0644; uid/gid/mtime 0; no extensions; only directories and regular
files), so the same save always gives the same blob (same `sha256_ciphertext`). The synthetic nonce never repeats for two different saves
under one version key. Limits: 64 MiB per blob, 256 MiB of files per archive; the reader rejects absolute paths and `..`.

## Local state

`<support dir>/cloud/<namespace>/` (support dir = `NAPPLE_SUPPORT_DIR`, else `~/Library/Application Support/Napple`; iOS: the app's save
root): `account.json`, `sync/<TITLE>-<profile>.json` (the version this device is based on + the hash of that save), `queue.json`,
`restore/<TITLE>/<profile>/*.tar.zst`, `status/<TITLE>.json` + `events.jsonl` (events for the frontend), `lock`.

## Environment

| Variable | Meaning |
|---|---|
| `NAPPLE_ACCOUNT_URL` | server base URL (default `https://pretend.napple.lat/account/v1`) |
| `NAPPLE_ACCOUNT_NAMESPACE` | another sign-in on this machine (state dir and Keychain service); default `default` |
| `NAPPLE_ACCOUNT_KEYCHAIN` / `_PASSWORD` | macOS: use this keychain file instead of the login keychain (created on first use) |
| `NAPPLE_ACCOUNT_STORE=file` | secrets in 0600 files under the state dir instead of the Keychain (Linux, CI) |
| `NAPPLE_ACCOUNT_TIMEOUT` | per-request timeout in seconds (default 20) |
| `NAPPLE_CLOUD=0` | runtime: no cloud sync for this run |
| `NAPPLE_CLOUD_PULL_TIMEOUT_MS` | runtime: pull-before-mount budget (default 8000) |
| `NAPPLE_CLOUD_DEBOUNCE_MS` | runtime: quiet time after a commit before the upload (default 5000) |
| `NAPPLE_CLOUD_EXIT_TIMEOUT_MS` | runtime: upload budget at a clean exit (default 10000) |

## Developing and testing

```bash
cargo test -p napple-cloud                       # crypto vectors, tar, blob, Keychain wrapper, two-device end to end against the stub
cargo run -p napple-cloud --features stub --bin napple-cloud-stub -- 127.0.0.1:8787 --code 123456
NAPPLE_ACCOUNT_URL=http://127.0.0.1:8787/account/v1 napple account login --email me@example.com --code 123456
```
Two devices on one Mac: give each its own `NAPPLE_ACCOUNT_NAMESPACE`, `NAPPLE_ACCOUNT_KEYCHAIN` file and runtime `--save-root`.
The runtime links `target/release/libnapple_cloud.a` (`make -C runtime`; `CLOUD=0` builds without it; the hooks in `main.c` and
`services/fs.c` are weak no-ops then).
