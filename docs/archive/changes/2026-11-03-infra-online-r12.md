# infra-online (r12): social + account in the deployed stack, SES, photo and save buckets

- New images `napple-account` and `napple-social` (Dockerfiles + ignore files), ECR repositories, deploy matrix, workflow path filters, roll script (10 arguments), smoke test (`/account/healthz`, `/social/healthz`).
- Compose: `account` (8082) and `social` (8083) services with the contract env names (also the pre-r12 napple-account names while the crate still reads them), `online` gets `NAPPLE_ACCOUNT_URL`/`NAPPLE_SOCIAL_URL`. Caddy: `/account/v1/*`, `/social/v1/*` (WebSocket upgrade forwarded), health aliases.
- OpenTofu: private photo and saves buckets (`storage.tf`), instance role gets SES send (identity `napple.lat`, config set `napple-transactional`) and object access on the two buckets; mocked tests extended.
- `infra/local/run.sh` starts account + social too (dev mailer) and prints `NAPPLE_ACCOUNT_URL` / `NAPPLE_SOCIAL_URL`; stub server and stub image know the new roles.
- Verified: `tofu fmt/validate/test` (9 pass), actionlint, shellcheck, `run.sh --stub` start/status/stop. Not run: docker (absent), tofu plan/apply (no AWS changes made).
