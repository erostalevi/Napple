---
name: infra-online
batch: 2026-11-03-r12
branch: claude/infra-online-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_2cca500c-4401-46cd-a061-3a3602c54337
launched_at: 2026-11-03
---

# infra-online: deploy social, Caddy routes, SES permissions, photo storage, env wiring

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
`infra/**`, `.github/workflows/deploy-servers.yml`, `docs/guides/deploy-servers.md`, change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Add napple-social (and the account/online services as already present) to the container build, compose and Caddy routes per the contract (`/account/v1`, `/social/v1` incl. WebSocket, online as today); image build jobs; health checks; env wiring from the contract (names exactly).
2. IAM (instance role): `ses:SendEmail`/`SendRawEmail` on the napple.lat identity and config set `napple-transactional`; S3 bucket for photos (private, block public access, lifecycle) and `NAPPLE_PHOTO_BUCKET`; keep least privilege; the deploy role already has PowerUserAccess + napple-* IAM.
3. Mail env: `NAPPLE_MAIL_PROVIDER=ses`, `NAPPLE_MAIL_FROM=no-reply@napple.lat`, `NAPPLE_SES_CONFIG_SET=napple-transactional` as defaults in compose; secrets (if any) via SSM Parameter Store.
4. Extend `infra/local/run.sh` to start social too (and print the env to point clients at it).
5. Offline checks: `tofu fmt/validate/test`, actionlint, shellcheck, `docker compose config` if available. Do NOT run apply and make NO AWS changes yourself (read-only CLI checks with AWS_PROFILE=napple are fine); the orchestrator publishes and the GitHub workflow deploys. Note: SES production access is pending; the sandbox only delivers to verified recipients.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
