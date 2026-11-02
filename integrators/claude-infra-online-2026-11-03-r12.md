---
branch: claude/infra-online-2026-11-03-r12
orchestration: infra-online
base: ac6692cb300be6fc6e90eb894de0268c3732e948
head: 7e4024cf49152c4bc032b25de8c49b27378b19ec
env: mac
date: 2026-11-03
areas: [infra, .github/workflows/deploy-servers.yml, docs/guides/deploy-servers.md]
status: ready-with-caveats
---

# claude/infra-online-2026-11-03-r12: deploy napple-account + napple-social, SES, photo/saves buckets, env wiring

## Summary
The deployed stack now runs four services behind Caddy: online (8080), room, **account** (8082) and **social** (8083). New Dockerfiles, ECR repos,
image matrix, workflow path filters, a 10-argument `roll.sh`, smoke checks for `/account/healthz` and `/social/v1/healthz`. Caddy routes
`/account/v1/*` and `/social/v1/*` (prefix kept; the social WebSocket upgrade is forwarded) and everything else to online as before. OpenTofu adds two
private S3 buckets (photos, saves: Block Public Access, TLS-only policy, SSE-S3), instance-role permissions (SES send on the `napple.lat` identity +
`napple-transactional` config set only; object access on the two buckets only) and mocked tests. Compose defaults: `NAPPLE_MAIL_PROVIDER=ses`,
`NAPPLE_MAIL_FROM=no-reply@napple.lat`, `NAPPLE_SES_CONFIG_SET=napple-transactional`. `infra/local/run.sh` starts account + social (dev mailer) and prints
`NAPPLE_ACCOUNT_URL`/`NAPPLE_SOCIAL_URL`. No AWS change was made; no secrets needed (SES/S3 via the instance role).

## Files
`infra/terraform/{storage.tf (new),iam.tf,ecr.tf,outputs.tf,variables.tf,tests/servers.tftest.hcl}`, `infra/docker/{compose*.yml,Caddyfile,stub.Dockerfile,napple-account.Dockerfile*,napple-social.Dockerfile*}`,
`infra/scripts/{roll.sh,smoke.sh,tofu-servers.sh}`, `infra/local/{run.sh,stub-server.py}`, `.github/workflows/deploy-servers.yml`,
`docs/guides/deploy-servers.md`, `docs/archive/changes/2026-11-03-infra-online-r12.md`. No shared files touched.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| tofu fmt / validate / test | tested | `tofu validate` ok; `tofu test`: 9 passed (new run `online_suite`) |
| actionlint, shellcheck | tested | clean on the workflow and `infra/scripts/*.sh infra/local/run.sh` |
| run.sh with stand-ins | verified | `run.sh start --stub`: 4 healthz OK, status, stop |
| run.sh with the real crates | verified | `CARGO_BUILD_JOBS=2 run.sh start`: account, social (`/social/v1/healthz`), online, room all healthy; stop OK |
| Dockerfiles, compose, Caddyfile | untested | no docker/caddy on this Mac; Dockerfiles are copies of the online one with names/ports changed |
| SES identity/config set exist | verified (read-only) | `AWS_PROFILE=napple aws sesv2 list-email-identities` -> napple.lat; config set napple-transactional |

## Unbuilt / untested list
- `infra/docker/napple-account.Dockerfile`, `napple-social.Dockerfile`, `compose.yml` account/social services, `Caddyfile` new routes (never run in Docker/Caddy).
- `storage.tf`/`iam.tf` plan against real AWS (only mocked).

## Findings
- napple-account on this base still reads the pre-r12 env names (`NAPPLE_ACCOUNT_HTTP`, `_DATA`, `_MAILER`, `_ENV`, `_S3_BUCKET`, `_MAIL_FROM`, `_SES_REGION`).
  Compose and run.sh set the **contract** names (`NAPPLE_ACCOUNT_BIND`, `_DATA_DIR`, `NAPPLE_MAIL_*`, `NAPPLE_SES_*`, `NAPPLE_PHOTO_BUCKET`) AND the old ones
  (commented as such) so the stack works before and after account-auth-ses lands; delete the old lines later. The saves bucket is wired as `NAPPLE_ACCOUNT_S3_BUCKET`
  (env var of the existing blob store; the contract has no name for it): account-auth-ses/profiles should keep that name or tell infra.
- Compose sets `NAPPLE_ACCOUNT_ENV=production` (the crate then demands the ses mailer and an S3 bucket). Local Docker needs `NAPPLE_ACCOUNT_ENV=development NAPPLE_MAIL_PROVIDER=dev`.
- social's `CMD ["serve"]` argument is ignored by its main (no arg parsing). Health: account `/healthz` (root, also `/account/healthz` via Caddy); social only has `/social/v1/healthz` (no root one), used by its Dockerfile/smoke/run.sh.
- Decision: online reaches account/social over the compose network (`http://account:8082`), not via the public name, to avoid hairpin TLS. `NAPPLE_SOCIAL_TRUST_PROXY=1`, `NAPPLE_ACCOUNT_TRUST_PROXY=1`.
- Decision: state dirs for account/social are created by `roll.sh` (not user_data) so the instance is not touched by this change.
- Deploy-role caveat: the S3 buckets `napple-photos-*`/`napple-saves-*` and SES permissions rely on PowerUserAccess (as the task says). If the role is ever narrowed, `infra/aws/deploy-policy.json` needs `s3:*` on those names; it was not edited.
- SES production access is pending: only verified recipients get mail until then (account has talevineto@gmail.com verified).
- `destroy` now also force-empties the two buckets (photos and saves are lost): same semantic as the existing "accounts gone" warning.

## How to integrate
- Land after/with the crates; works with the current crates too. The first run of the workflow after publishing creates the buckets, ECR repos and rolls 4 images; images job builds the stand-in for any crate missing.
- No conflict hotspots outside infra. Build: nothing; `cd infra/terraform && tofu init -backend=false && tofu validate && tofu test`.
- No Linux-only calls.

## How to test
1. `cd infra/terraform && tofu init -backend=false -input=false && tofu validate && tofu test` -> 9 passed.
2. `actionlint .github/workflows/deploy-servers.yml && shellcheck -x infra/scripts/*.sh infra/local/run.sh`.
3. `infra/local/run.sh start --stub` -> four healthy lines; `infra/local/run.sh stop`. With real crates: `CARGO_BUILD_JOBS=2 infra/local/run.sh start`, codes in `$TMPDIR/napple-local-stack/logs/account.log`.
4. After deploy: `infra/scripts/smoke.sh https://pretend.napple.lat` (four endpoints) and a signup mail to a verified address.

## Fix policy hints
YAML/Caddy syntax slips are safe to fix. Changing route prefixes or env names needs a contract decision.

## Needs from other areas
- account-auth-ses: keep reading `NAPPLE_PHOTO_BUCKET` / `NAPPLE_ACCOUNT_S3_BUCKET` and the contract mail names; bind on `NAPPLE_ACCOUNT_BIND`.
- napple-online (online-identity): read `NAPPLE_ACCOUNT_URL` / `NAPPLE_SOCIAL_URL`.
