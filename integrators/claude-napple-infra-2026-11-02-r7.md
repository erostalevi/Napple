---
branch: claude/napple-infra-2026-11-02-r7
orchestration: napple-infra
base: 91c5b2e71593aacc7c4cd8de613a810b0474b13d
head: see `git log -1 claude/napple-infra-2026-11-02-r7` (this file is in the head commit)
env: mac
date: 2026-11-02
areas: [infra, ci, docs, site]
status: ready-with-caveats
---

# claude/napple-infra-2026-11-02-r7: local test stack + AWS auto-deploy of the Napple game servers (pretend.napple.lat) and DNS for napple.lat

## Summary
Everything needed to deploy `napple-online` and `napple-room` to AWS once the user adds credentials, plus the `napple.lat` DNS:
- Dockerfiles (multi-stage Rust, non-root, arm64/amd64, `/healthz` checks) and a compose stack with Caddy terminating TLS for `pretend.napple.lat`.
- OpenTofu: a `bootstrap/` stack (S3 state bucket; locking with S3 lock files, no DynamoDB) and the servers stack (own VPC, SG without SSH,
  t4g.micro AL2023 with Docker + Compose, EIP, an 8 GB root + 1 GB persistent encrypted data volume, instance role for SSM/ECR/logs, ECR repositories, log group, Route 53 records in the existing zone,
  looked up by id or name: `pretend` A, apex A/AAAA to Pages, `www` + `*` CNAME to `erostalevi.github.io`, optional Pages verification TXT).
- Names follow the orchestrator's contract (the user set AWS/GitHub up with these): secret `AWS_DEPLOY_ROLE_ARN` (OIDC role `napple-deploy`) with the
  `AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY` fallback; variables `AWS_REGION`, `NAPPLE_DOMAIN`, `NAPPLE_ZONE_ID` (zone by id when set, else by name);
  every AWS job runs in environment `napple-servers`; dispatch modes `bootstrap`, `plan`, `apply`, `destroy`; IAM resources `napple-*`
  (instance role/profile `napple-servers-instance`).
- `.github/workflows/deploy-servers.yml`: a guard job skips cleanly (green run, notice in the summary) without `AWS_DEPLOY_ROLE_ARN` or both
  access-key secrets. It no longer waits for the crates: while `crates/<name>` is missing, a stand-in image (`infra/docker/stub.Dockerfile`,
  the stand-in server answering `/healthz`) is built and deployed in its place, and the first push that brings the crate replaces it. After that come bootstrap (dispatch), plan/apply, native arm64 image builds to ECR, the SSM roll
  and the smoke test of `/healthz` + `/room/healthz`. Rollback is a dispatch with `image_tag`; `destroy` (with `confirm=destroy`) removes the servers
  and keeps the hosted zone and the GitHub Pages records.
- `infra/local/run.sh`: native local stack (no Docker), with `--stub` / `--bin-dir` for testing before the crates exist.
- Go-live cost pass (orchestrator, for the user): t4g.micro (free-tier eligible on account 785060334352), 8 GB gp3 root (the AMI snapshot size),
  1 GB data volume (`data_volume_gb = 0` drops it), standard CPU credits, no NAT/LB/detailed monitoring/Spot, ECR keeps 3 images, logs 3 days,
  no DynamoDB; about $11/month at list price, the instance hours under the free tier (estimate in the guide).
- `site/CNAME` = `www.napple.lat`; guide `docs/guides/deploy-servers.md` (one-time setup, OIDC trust policy, costs, rollback/destroy, security).
NOT done: acceptance 3's "a Napple runtime pointed at it reaches them". Neither `crates/napple-online` nor `crates/napple-room` was committed on its
branch at hand-off (napple-room had only `src/proto.rs` uncommitted, napple-online had no crate), so the stack was tested against the stand-in server
only. `app.napple.lat` is deliberately not built.

## Files
`git diff --stat 91c5b2e..HEAD`: all new except the link lines.
- New: `infra/**` (README, .gitignore, docker/ 9 files, terraform/ 10 files + bootstrap/ 5 files + 2 test files + 2 provider lock files, aws/ 2 JSON,
  scripts/ 5, local/ 2), `.github/workflows/deploy-servers.yml`, `site/CNAME`, `docs/guides/deploy-servers.md`,
  `docs/archive/changes/2026-11-02-claude-napple-infra-2026-11-02-r7.md`, this file.
- Shared, append-only: `docs/guides/README.md` (one table row), `docs/index.md` (one line, Maintainer list item 4),
  `docs/archive/changes/README.md` (new `### Round 2026-11-02-r7` section with one row).
- Not touched: `crates/`, `runtime/`, other workflows, `orchestrations/`, `Cargo.toml`.

## Built and tested here
| Item | Status | Evidence (command + result) |
|---|---|---|
| Acceptance 1: `tofu fmt -check`, `validate`, plan against a mock provider, both stacks | tested | `cd infra/terraform && tofu init -backend=false && tofu validate && tofu test` gives 8 passed (defaults incl. `napple-*` IAM names, cost_defaults, no_data_volume, pages_off_and_verification, zone_by_id, zone_by_name, x86_64, bad_architecture); `cd bootstrap && …` gives 2 passed. OpenTofu 1.13.5, hashicorp/aws 6.71.0. `mock_provider "aws"`, so no AWS call. |
| Acceptance 1: actionlint on the workflow | tested | `actionlint .github/workflows/deploy-servers.yml`: no findings (actionlint 1.7.12, includes shellcheck of the run blocks). |
| Acceptance 2: skips cleanly without secrets | tested | Guard script extracted and run: no secrets → `run=false` + notice; only the key id → skip; role with the crates missing (this tree) → `run=true` (stand-ins); dispatch apply off main → skip; plan off main → `run=true`; bootstrap with keys on main → `run=true`; destroy without `confirm` → skip; destroy confirmed → `run=true`. The images job's source step: no crate → `stub.Dockerfile`, context `infra/local`, `ROLE=online`/`room`; crate present → the real Dockerfile, context `.`. Every later job requires `run == 'true'` (`roll` also requires `infra` success). `act` not installed, not used. |
| Acceptance 3: `infra/local/run.sh` starts both servers | verified (stand-in only) | Under `/bin/bash` 3.2: `run.sh start --stub` brings both up healthy; curl `/healthz` OK; UDP echo on 60000/60001; TCP echo on 31415; second start refused (exit 1); `stop` leaves no process; `--bin-dir` + custom ports; `KEEP_DATA=1`; a crashing binary makes start fail and clean up; without the crates it exits with "crates/napple-online is not in this checkout". |
| Acceptance 3: a Napple runtime reaches them | not done | The crates and the client side were not committed on their branches (see Summary). |
| Acceptance 4: no AWS call from this Mac | verified | No AWS CLI calls, no apply; `tofu test` runs with a mocked provider. The deploy scripts refuse to run outside Actions (`require_ci`). No credentials written anywhere. |
| Stand-in image contents | tested (without Docker) | The container's entrypoint (`sh -c 'exec python3 stub-server.py "$NAPPLE_STUB_ROLE"'`) with the compose env (0.0.0.0 binds) and its HEALTHCHECK command run natively: both health checks pass, `/healthz` → `ok stub online`, `/healthz` on 8081 → `ok stub room`, SIGTERM stops both. The image itself was never built. |
| `use_lockfile` with the pinned OpenTofu | tested | `tofu init` (1.13.5) of an `s3` backend with `-backend-config=use_lockfile=true` and the endpoints pointed at 127.0.0.1:9: the option is accepted (fails only on the unreachable endpoint); a bogus option is rejected as "not expected". No AWS call. |
| Read-only AWS checks (allowed by the orchestrator, `AWS_PROFILE=napple`) | verified | `sts get-caller-identity` → account 785060334352, user napple-claude; free-tier-eligible types include t4g.micro and t4g.small; zone Z01748463IB00LNMM64RG `napple.lat.` public with 4 NS; existing records apex A (4 Pages IPs), AAAA (4), `www` and `*` CNAME `erostalevi.github.io`, all TTL 300 = the stack's values; AL2023 arm64 AMI snapshot = 8 GB; role `napple-deploy` exists (see Findings); no `napple*` bucket yet, no EIP; only the default VPC. Nothing created or changed. |
| Compose files | tested | `docker-compose config -q` (Compose 5.6.1 CLI only, no Docker engine) for `compose.yml`, `+compose.build.yml`, `+compose.aws.yml`: valid; `!override` volumes merge as intended. |
| Deploy scripts + rendered user data | tested (lint) | `shellcheck -x infra/scripts/*.sh infra/local/run.sh` clean; `user_data.sh.tftpl` rendered with sed then shellchecked clean. |
| Docker image builds | untested | No Docker on this Mac (the assignment says don't install it). |
| AWS apply / SSM roll / Caddy certificate / smoke test | untested | Needs the user's AWS account. |
| Docs | tested | `python3 tools/check_docs.py`: 248 files OK. |
Evidence files: `side-session-notes/local_67fc7cb9-…/napple-infra-evidence/` (tofu-test.txt, lint.txt, local-stack-*.txt, guard.sh).

## Unbuilt / untested list (the integrator checks these first)
- `infra/docker/napple-online.Dockerfile`, `infra/docker/napple-room.Dockerfile`: never built (no Docker). Commit 38f1958.
- `infra/scripts/bootstrap.sh`, `tofu-servers.sh`, `roll.sh`, `smoke.sh`: lint-clean, never run against AWS. Commit a3e4ebb.
- `infra/terraform/user_data.sh.tftpl`: never booted on an instance. Commit 1924402.
- The `images` and `roll` jobs of the workflow: never run.
- `infra/docker/stub.Dockerfile`: never built (no Docker).
- The `destroy` path of `infra/scripts/tofu-servers.sh` (targeted apply of `ecr_force_delete`, then `tofu destroy -exclude=<Pages records>`): never run;
  `-exclude` exists in the pinned OpenTofu 1.13.5 (`tofu destroy -help`).

## Findings
- **Decision: the server contract.** The crates did not exist yet, so I fixed the env/ports the infra assumes and sent it to both sessions
  (napple-online, net-rooms) plus the shared note `napple-infra-server-contract.md`. `NAPPLE_ONLINE_HTTP` :8080 with `/healthz`,
  `NAPPLE_ONLINE_NEX_AUTH` :60000/udp, `NAPPLE_ONLINE_NEX_SECURE` :60001/udp, `NAPPLE_ONLINE_PUBLIC_HOST/IP`, `NAPPLE_ONLINE_DATA`;
  `NAPPLE_ROOM_LISTEN` :31415 (tcp, net-rooms' `proto::DEFAULT_PORT`), `NAPPLE_ROOM_HTTP` :8081 with `/healthz`, `NAPPLE_ROOM_DATA`. Full table in
  `docs/guides/deploy-servers.md#server-contract`. If the crates chose differently, see "How to integrate".
- **Decision: the wildcard.** Kept `*.napple.lat` CNAME to `<owner>.github.io` as the user planned, but GitHub (a) serves only the configured custom domain,
  so `app.napple.lat` gets GitHub's "site not found" rather than the landing page, and (b) warns that a wildcard to github.io allows subdomain
  takeover unless the domain is verified in the account. The guide makes verification step 2; variable `wildcard_to_pages` turns it off; repo var
  `NAPPLE_PAGES_VERIFICATION_TXT` creates the TXT.
- **Decision: deploy mechanics.** User data only installs Docker/Compose/the data volume; each deploy writes the compose files + `.env` through SSM
  `send-command` and pulls images by commit SHA (immutable ECR tags). This makes rollback a re-roll to an older tag, and a replaced instance comes
  back on the next run. Instance `ignore_changes = [ami]` so new AMIs don't replace the server unasked.
- **Decision: destroy keeps the Pages records.** "destroy keeps the hosted zone; only stack resources": the apex/`www`/wildcard/TXT records are
  stack resources, but deleting them takes the landing page down, so `destroy` excludes them (`-exclude`) and removes everything else.
  Delete them by hand (or `tofu destroy` without the excludes) if that is ever wanted.
- **Decision: the guard job runs in `napple-servers` too**, so secrets/variables stored on the environment (not only on the repo) are visible to it,
  and it hands the resolved variables to the other jobs. Consequence: a `plan` from a non-`main` branch is blocked by the environment's branch
  rule before the guard can skip it (documented in Troubleshooting).
- **Decision: the deploy policy** in `infra/aws/deploy-policy.json` now allows `role/napple-*` and `instance-profile/napple-*` as the orchestrator
  specified, plus an explicit deny of `iam:*` on `role/napple-deploy` so the deploy role cannot widen its own rights.
- **Decision: own VPC** (no reliance on a default VPC), no NAT. **Separate EBS data volume** so accounts survive instance replacement.
- **Decision: state bootstrap.** The bootstrap stack starts with local state; `bootstrap.sh` then writes `backend_override.tf` and migrates its state into the
  bucket it created, so later runs are idempotent. The servers stack uses a partial `backend "s3" {}` filled from the account id + region.
- **Decision: Compose plugin 5.6.1** (current; the assets `docker-compose-linux-{aarch64,x86_64}` + `.sha256` checked on GitHub), verified at boot against the release
  checksum file (integrity in transit only, not a signature).
- `configure-aws-credentials` gets `mask-aws-account-id: false`: masked values (the ECR URLs contain the account id) can't pass as job outputs.
- **Go-live blockers on the AWS side, reported to the orchestrator (not mine to fix; I made no AWS change):**
  (1) the `napple-deploy` trust policy's subject is `StringLike ["repo:napple-deploy/*", ...]`, which never matches GitHub's token; it must be
  `repo:erostalevi/napple:environment:napple-servers` and `repo:erostalevi/napple:ref:refs/heads/main` (`infra/aws/github-oidc-trust.json`).
  (2) `napple-deploy` has only an IAM-only inline policy (`napple-deployPolicy`, role/instance-profile `napple-*`) and no managed policies: no S3,
  EC2, SSM, ECR, Logs, Route 53 or sts. `infra/aws/deploy-policy.json` is the full set (now without DynamoDB).
- **Update: both blockers fixed by the orchestrator** (re-checked read-only): the trust policy now matches the environment and `main` subjects in both
  casings (`erostalevi/napple`, `erostalevi/Napple`: the repository was renamed), and `PowerUserAccess` is attached next to the inline IAM policy.
  `infra/aws/github-oidc-trust.json` lists both casings now; `deploy-policy.json` stays as an optional least-privilege replacement.
- **Decision: take over the hand-made DNS records.** The zone already holds apex A/AAAA, `www` and `*` with exactly the stack's values, so
  `dns_allow_overwrite` now defaults to `true` (workflow variable fallback `true`); otherwise the first apply would fail with "already exists".
- **Decision: S3 lock files** instead of DynamoDB (the orchestrator asked for fewer resources; supported by the pinned 1.13.5, see above).
- **Decision: the stand-in.** One small image (`python:3.13-alpine` + `stub-server.py`), built per role into the real repositories with the
  commit's tag, so the deploy path is identical to the real one; it stores nothing and speaks no game protocol.
- **Decision: 1 GB data volume kept** (the orchestrator allowed "or make it 1 GB"): accounts and certificates survive an instance replacement for
  about $0.08/month (inside the free tier's 30 GB).
- Outside my area, not fixed: `.github/workflows/release.yml:70` has an actionlint/shellcheck SC2046 warning (unquoted `$(...)`).
- Outside my area, not fixed: with Pages on `www.napple.lat`, `site/app.js` can no longer derive owner/repo from a `*.github.io` host
  (`site/index.html` has `<meta name="napple-repo" content="">`), so the landing page's release/download lookups will fail. `og:image` still points at
  `erostalevi.github.io/Napple/…`. See Needs.
- Both sibling branches (`claude/napple-online-2026-11-02-r7`, `claude/net-rooms-2026-11-02-r7`) sat at `c44af84` with no commits when I finished.

## How to integrate
- Merge order: after `claude/napple-online-2026-11-02-r7` and `claude/net-rooms-2026-11-02-r7` (this branch only references their crates by name).
  It has no code dependency, so it can land first: the workflow simply skips while the crates are missing.
- Conflict hotspots: `docs/archive/changes/README.md` (the other r7 branches will add the same `### Round 2026-11-02-r7` heading: keep one heading and all rows),
  `docs/guides/README.md` (others may add rows: keep all), `docs/index.md`.
- **Contract check after the server branches land** (the main integration step):
  `grep -rn 'env::var\|NAPPLE_' crates/napple-online/src crates/napple-room/src`, and compare with the table in `docs/guides/deploy-servers.md#server-contract`.
  Where they differ, change the env names/ports in `infra/docker/compose.yml`, the two Dockerfiles (`ENV`, `EXPOSE`, `HEALTHCHECK`), `infra/docker/Caddyfile`
  (upstream ports), `infra/local/run.sh` (the two launch lines), `infra/terraform/variables.tf` (`nex_udp_*`, `room_port`) and the guide's table. If a crate
  has no `/healthz`, ask its owner rather than dropping the health checks. If the binary name differs, update `--bin` in the Dockerfile.
- Build/check commands, in order:
  1. `cd infra/terraform && tofu init -backend=false && tofu validate && tofu test` → 6 passed; same in `infra/terraform/bootstrap` → 2 passed.
  2. `actionlint .github/workflows/deploy-servers.yml` → silent.
  3. `infra/local/run.sh` (with the crates merged) → "online healthy", "room healthy" and the env block; `infra/local/run.sh stop`.
  4. `python3 tools/check_docs.py` → OK.
- No Linux-only calls in product code. The scripts in `infra/scripts/` are meant for the Ubuntu runners (bash 5, `mapfile`, GNU `base64`); `infra/local/run.sh`
  is written for macOS `/bin/bash` 3.2.

## How to test
1. Offline IaC tests: commands 1-2 above. Passing looks like `Success! 6 passed, 0 failed.`; failing names the assert.
2. Local stack with the real servers: `infra/local/run.sh`, then
   `NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://127.0.0.1:18080 NAPPLE_ROOM=127.0.0.1:31415` on a headless MK8D run under the run lock (≤120 s). Passing:
   the server logs (`infra/local/run.sh logs`) show the runtime's token request and/or room HELLO. Failing: no request lines, so check the
   client env names the server sessions chose.
3. Without Docker: `infra/local/run.sh --stub` → both healthy; this proves only the script.
4. With Docker (optional, any machine): `cd infra/docker && NAPPLE_DOMAIN=localhost docker compose -f compose.yml -f compose.build.yml up --build`, then
   `curl -k https://localhost/healthz` and `curl -k https://localhost/room/healthz`.
5. Needs a human (the user): the one-time setup in `docs/guides/deploy-servers.md` (zone, Pages domain verification + custom domain, OIDC role or keys),
   then Actions > deploy-servers > mode `bootstrap`, `plan`, `apply`. Passing before the crates land: the summary shows the two stand-in notices and the
   smoke step prints `ok stub online` and `ok stub room` from `https://pretend.napple.lat`.
6. Regression guard: nothing outside `infra/` and the new workflow changes behaviour; `tests.yml`/`pages.yml` untouched. Pages keeps working: `site/CNAME` is only copied into the artifact.

## Fix policy hints
- Safe to fix directly: env var/port names to match the crates (see the contract check), typos, a Rust image tag that does not exist (`RUST_IMAGE` ARG),
  the Compose version, the OpenTofu version in the workflow.
- Needs a decision: dropping the wildcard (security vs the user's plan; it's a variable); serving `app.napple.lat`/the wildcard from Caddy as a
  redirect instead of GitHub Pages (needs on-demand TLS); an amd64 instance (set repo variables `NAPPLE_ARCHITECTURE=x86_64` and
  `NAPPLE_INSTANCE_TYPE=t3.small`; nothing else changes).

## Needs from other areas
- **site/** (landing page owner): set `<meta name="napple-repo" content="erostalevi/napple">` in `site/index.html` (the custom domain breaks the
  github.io-based derivation in `site/app.js`) and change `og:image` to `https://www.napple.lat/assets/social.png`.
- **napple-online** (crate): follow or document the contract above; plain-HTTP API + `/healthz`; build on Linux without OpenSSL; take the public
  IP for NEX station URLs from `NAPPLE_ONLINE_PUBLIC_IP`.
- **net-rooms** (crate): an HTTP `/healthz` listener (`NAPPLE_ROOM_HTTP`) next to the TCP room port, and env-driven listen address.
- **docs-site/mkdocs.yml** (optional): a nav line for `guides/deploy-servers.md` (several guides are already absent from the nav, so this is cosmetic).
