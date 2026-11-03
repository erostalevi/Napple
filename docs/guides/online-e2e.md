# Napple Online end-to-end

`tools/e2e/online/online_e2e.py` checks the whole Napple Online flow with the real client core (`crates/napple-cloud/examples/online_cli`, the
same code the overlay's function table calls): sign-up with an emailed code, verify, a second device with end-to-end key approval, profile /
photo / tier, a cloud save written on device A and pulled on device B before mount, friends between two accounts, every tier cell for search /
friend request / presence, logout and account deletion. It prints a PASS/FAIL table (60 checks) and exits 1 on any FAIL.

## Local (dev mailer)
```bash
cargo build -p napple-cloud --example online_cli
NAPPLE_SOCIAL_PROFILE_TTL_SECS=2 infra/local/run.sh start        # fast tier snapshots; sign-up mail limits are per hour: restart between runs
python3 tools/e2e/online/online_e2e.py
infra/local/run.sh stop
```

## Live (https://pretend.napple.lat)
`python3 tools/e2e/online/online_e2e.py --live` with `E2E_CODE_DIR=<dir>`: the script signs up `tieup-{a,b,c}<tag>@napple.lat` and waits for a
file `<dir>/<address>` holding the newest 6-digit code (read from the forwarded mail; while SES is in its sandbox only verified recipients and
`@napple.lat` can be mailed, and inbound `*@napple.lat` is forwarded). Tier steps wait 11 s (napple-social trusts a profile snapshot for 10 s).
All accounts are deleted at the end.

## Overlay with the real client
`tools/e2e/online/build_overlay_shot.sh`, then `OVERLAY_EMAIL=.. OVERLAY_PASSWORD=.. NAPPLE_HOME=<empty dir> OVERLAY_SHOT_DIR=<dir> $TMPDIR/overlay_shot`:
the Online tab signs in through its own buttons over `napple_online_runtime_start` and renders every page to PNG.

## Game side
A signed-in Mac plays as its profile: the NEX principal id is `identity::principal_id(profile)`, the ID token is the account access token, and
friends and presence come from napple-social (`net_online_identity` test, `test_online_ui`). A headless boot of an online title reaches "local user
linked", presence "playing" and the friend service; a full match needs a person at the game's menus.
