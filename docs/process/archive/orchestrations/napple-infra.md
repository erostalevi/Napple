---
name: napple-infra
batch: 2026-11-02-r7
branch: claude/napple-infra-2026-11-02-r7
base: orchestrate/2026-11-02-r7
env: mac
model: claude-opus-5-5
depends_on: []   # wires to the napple-online / napple-room crates by name; integrate after them
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_53c5ec03-3f03-47b6-80e9-ee3d9cb67e20 (fresh)
launched_at: 2026-11-02
---

# napple-infra: local test stack + AWS auto-deploy of the Napple game servers (pretend.napple.lat) and DNS for napple.lat

## Goal
The user bought `napple.lat` and will create its Route 53 hosted zone, then add AWS credentials to the GitHub repo. Prepare everything so that, once the secrets exist, a push to `main` deploys the Napple servers automatically. Until then nothing deploys and the workflow skips cleanly.
DNS plan (user's):
- `pretend.napple.lat` -> the Napple game servers: `napple-online` (Pretendo-style accounts/tokens + NEX game server, crate `crates/napple-online`, being built by session napple-online) and `napple-room` (LDN/LAN Play relay, crate `crates/napple-room`, being built by session net-rooms).
- `www.napple.lat`, apex `napple.lat` and wildcard `*.napple.lat` -> the existing GitHub Pages landing page (`site/`, `.github/workflows/pages.yml`).
- `app.napple.lat`: NOT built (hosted upload of games/keys is out of scope by decision); it falls under the wildcard to the landing page.

## Deliverables
1. **Containers**: `infra/docker/napple-online.Dockerfile`, `infra/docker/napple-room.Dockerfile` (multi-stage Rust builds, small runtime images, non-root, linux/arm64 + amd64), `infra/docker/compose.yml` running both plus a TLS terminator (Caddy with automatic certificates for `pretend.napple.lat`) for the HTTPS account/token API; UDP ports for PRUDP/NEX and the room relay published directly. Health checks.
2. **Terraform/OpenTofu** in `infra/terraform/`: S3 backend + DynamoDB lock (separate `bootstrap/` stack, run once by a workflow_dispatch job), ECR repos for both images, one ARM EC2 instance (t4g.small default, variable) on Amazon Linux 2023 with Docker + compose via user data, Elastic IP, security group (80/443 TCP, the NEX UDP range, the room ports; SSH closed, SSM Session Manager instead), IAM instance role (SSM + ECR pull + CloudWatch logs), Route 53 records in the existing `napple.lat` zone (looked up by name, never created/destroyed by this stack): `pretend` A -> EIP; `www` CNAME -> `<owner>.github.io`; apex A -> GitHub Pages IPs (185.199.108-111.153) + AAAA; `*` CNAME -> `<owner>.github.io`. Variables for region (default us-east-1), domain, GitHub owner, instance type; outputs.
3. **Workflow** `.github/workflows/deploy-servers.yml`: on push to `main` touching `crates/napple-online/**`, `crates/napple-room/**`, `infra/**` (+ workflow_dispatch with an `apply`/`plan` choice and the bootstrap job). First step checks whether AWS credentials exist (OIDC role ARN secret `AWS_DEPLOY_ROLE_ARN` preferred, or `AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY`) and skips the rest with a notice if not. Then: build + push images to ECR (native arm64 runner `ubuntu-24.04-arm`, or buildx), `tofu/terraform init/plan/apply`, roll the instance via SSM `send-command` (`docker compose pull && up -d`), smoke-test `https://pretend.napple.lat/healthz`. Concurrency group, least-privilege notes.
4. **Pages custom domain**: `site/CNAME` = `www.napple.lat` (and document the one-time repo Settings > Pages custom-domain + HTTPS step; with Actions-based Pages the setting is what counts).
5. **Local stack**: `infra/local/run.sh` (native, no Docker needed on this Mac): builds and starts `napple-online` + `napple-room` on localhost with a throwaway config, prints the env to point Napple at it (`NAPPLE_NET=host`, `NAPPLE_ONLINE_SERVER=http://127.0.0.1:<port>`, `NAPPLE_ROOM=...`); stops cleanly.
6. **Docs**: `docs/guides/deploy-servers.md` — one-time AWS setup (hosted zone, OIDC role with the exact trust policy for this repo, or access keys; required secrets; bootstrap run), costs estimate, how to roll back/destroy, security notes (no game data or keys ever on the server).

## Owned paths
`infra/` (new), `.github/workflows/deploy-servers.yml` (new), `site/CNAME` (new), `docs/guides/deploy-servers.md` (new), `docs/index.md` + `docs/guides/README.md` (link lines only), own change note.

## Do not touch
`crates/napple-online/`, `crates/napple-room/` (their sessions own them; if they lack a config knob or `/healthz`, write the need in "Needs from other areas" and message the orchestrator), `runtime/`, other workflows, `orchestrations/`.

## Acceptance
1. `tofu fmt -check`, `tofu validate` and `tofu plan` against a mock/empty provider config (or `-refresh=false` with dummy creds) pass for both stacks — install OpenTofu with `brew install opentofu` if absent (Terraform is no longer in homebrew-core). `actionlint` on the workflow if available (brew).
2. The workflow, run without secrets, would skip cleanly (show the guard logic; test it with `act` only if already installed — do not install Docker).
3. `infra/local/run.sh` starts both servers from the branches as they exist (merge `claude/napple-online-2026-11-02-r7` and `claude/net-rooms-2026-11-02-r7` into a scratch worktree for the check if they have crates by then; otherwise test the script against stub binaries and say so) and a Napple runtime pointed at it reaches them (log evidence).
4. No AWS call is made from this Mac (no credentials are configured; never run apply).

## Env notes
Read `orchestrations/ENV-MAC.md` (local-only, never ask, never push). Never write credentials anywhere.

## Result
Integrated 2026-11-02 into main as 14ad75c (head 9986727), outcome **Clean** (changes README rounds merged). Checked here: check_docs, bash -n, shellcheck, actionlint, tofu fmt, `infra/local/run.sh --stub` smoke. Not checked here: tofu test, Docker builds, AWS (no AWS call made). Acceptance 3's "a runtime reaches the real servers" waits for the server crates. Details: [integrator file](../integrators/claude-napple-infra-2026-11-02-r7.md).
