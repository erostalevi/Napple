# Deploy the Napple game servers (pretend.napple.lat) and the napple.lat DNS

**Goal:** once AWS credentials are in the repository's secrets, every push to `main` that touches the servers or `infra/` deploys
`napple-online` (NEX game server), `napple-account` (accounts, profiles, photos, cloud saves), `napple-social` (friends, presence, search, stats) and
`napple-room` (LDN / LAN Play relay) to one small AWS instance behind
`https://pretend.napple.lat`, and keeps the `napple.lat` records pointing at the GitHub Pages landing page. Until the secrets exist the workflow
skips with a notice and nothing is deployed.

Names the setup uses (match them exactly): secret `AWS_DEPLOY_ROLE_ARN` (OIDC role `napple-deploy`; fallback secrets `AWS_ACCESS_KEY_ID` +
`AWS_SECRET_ACCESS_KEY`), variables `AWS_REGION` (default `us-east-1`), `NAPPLE_DOMAIN` (default `napple.lat`), `NAPPLE_ZONE_ID` (optional),
GitHub environment `napple-servers` (every job that touches AWS runs in it), workflow modes `bootstrap`, `plan`, `apply`, `destroy`.
Every IAM resource the stack creates is named `napple-*` (the instance role and profile are `napple-servers-instance`).

Files: `infra/` (containers, OpenTofu stacks, deploy scripts, local stack), `.github/workflows/deploy-servers.yml`, `site/CNAME`.
`app.napple.lat` is deliberately **not** built (no hosted upload of games or keys); it falls under the wildcard record.

## What runs where

```
                         Route 53 zone napple.lat (created by you, looked up by the stack)
   napple.lat  A/AAAA ─┐  www  CNAME ─┐  *  CNAME ─┐                    pretend  A ──> Elastic IP
                       └──────────────┴────────────┴─> <owner>.github.io (site/ + /docs/)      │
                                                                                               v
   EC2 t4g.micro, Amazon Linux 2023, Docker Compose  (/opt/napple, state on a 1 GB EBS volume at /var/lib/napple)
     caddy  :80/:443 tcp, :443 udp ── TLS (automatic certificate for pretend.napple.lat)
        ├─ /room/*              -> room:8081     (napple-room status API, prefix stripped)
        ├─ /account/v1/*        -> account:8082  (napple-account; /account/healthz -> its /healthz)
        ├─ /social/v1/*         -> social:8083   (napple-social incl. the WebSocket /social/v1/stream; /social/healthz)
        └─ /*                   -> online:8080   (napple-online: /healthz, and the NEX game server as PRUDP Lite
                                                  over a WebSocket upgrade)
     online :10025, 10125, 33334, 33335 + the same +1000 /udp  Pia NAT check (published directly)
     room   :31415/tcp+udp    room protocol (published directly)
   Logs: CloudWatch group /napple/servers. Shell: SSM Session Manager (no SSH port).
```

**Data buckets and mail (round r12).** The stack creates two private S3 buckets (Block Public Access, TLS only, SSE-S3, no versioning, aborted
multipart uploads expire after 2 days): `napple-photos-<account>-<region>` and `napple-saves-<account>-<region>`. The instance role gets object
access on those two only, and `ses:SendEmail`/`ses:SendRawEmail` on the `napple.lat` identity and the `napple-transactional` configuration set only
(both exist in the account; `tofu` does not manage them). Photos never expire on their own: napple-account deletes them with the profile or account.

## DNS records

| Name | Type | Value | Notes |
|---|---|---|---|
| `pretend.napple.lat` | A | the Elastic IP | the game servers |
| `napple.lat` | A | 185.199.108.153, .109.153, .110.153, .111.153 | GitHub Pages; GitHub redirects the apex to `www` |
| `napple.lat` | AAAA | 2606:50c0:8000::153, 8001::153, 8002::153, 8003::153 | GitHub Pages |
| `www.napple.lat` | CNAME | `<owner>.github.io` | the Pages custom domain (`site/CNAME`) |
| `*.napple.lat` | CNAME | `<owner>.github.io` | everything else, `app` included (variable `wildcard_to_pages`) |
| `_github-pages-challenge-<owner>.napple.lat` | TXT | from GitHub | only when the repository variable `NAPPLE_PAGES_VERIFICATION_TXT` is set |

The hosted zone itself is never created or destroyed by the stack: it is looked up (`data "aws_route53_zone"`) by id when the variable
`NAPPLE_ZONE_ID` is set, else by the name in `NAPPLE_DOMAIN`. Record names follow `NAPPLE_DOMAIN` (`pretend.<domain>`, `www.<domain>`, ...). Records that already
exist with the same name and type are taken over (`allow_overwrite`): the `napple.lat` zone was set up by hand with exactly these Pages records.
Set the variable `NAPPLE_DNS_ALLOW_OVERWRITE` to `false` to make such a clash fail the apply instead.

**The wildcard and GitHub Pages.** GitHub only serves the one custom domain set in the repository (`www.napple.lat`); a request for
`app.napple.lat` reaching GitHub gets GitHub's "site not found" page, not the landing page. Worse, a wildcard pointing at `github.io` lets *any*
GitHub user claim `something.napple.lat` for their own Pages site unless the domain is **verified** in your account. So verify `napple.lat` first
(step 2 below). If you'd rather not, set `wildcard_to_pages = false` in `infra/terraform/variables.tf`, and unknown names stop resolving.

## Server contract

The Dockerfiles, compose file, Caddy config and `infra/local/run.sh` assume these binary names, env vars and ports. A server that picks
different names only needs those few files changed.

| Binary | Env var | Container default | Meaning |
|---|---|---|---|
| `napple-online serve` | `NAPPLE_ONLINE_HTTP` | `0.0.0.0:8080` | everything, over TCP: the account/token API, `GET /healthz` -> 200 `ok`, and the NEX game server (Switch NEX is PRUDP Lite inside a WebSocket: an upgrade on any other path). Caddy terminates TLS, forwards the upgrade and sets `X-Forwarded-For`, which the server reads for the client address. |
| | `NAPPLE_ONLINE_PUBLIC_HOST` | `pretend.napple.lat` | host name clients reach |
| | `NAPPLE_ONLINE_DATA` | `/var/lib/napple/online` | `accounts.json` and `server.key` (persistent volume) |
| | `NAPPLE_ONLINE_VERBOSE` | `0` | `1` = log every request |
| | `NAPPLE_ONLINE_NATCHECK_BASE` | `10025` | first UDP port of the Pia NAT check that titles run before matchmaking: UDP 10025, 10125, 33334, 33335 (primary server) and the same + 1000 (secondary), bound on the listener's address; `0` = off. The security group (`natcheck_udp_ports`) and the compose port list follow 10025: change all three together. |
| | `NAPPLE_ONLINE_PUBLIC_IP` | the Elastic IP | the address reported for a client on the server's own machine (from `.env`, written by each deploy) |
| `napple-account serve` | `NAPPLE_ACCOUNT_BIND` | `0.0.0.0:8082` | `/account/v1/*` and `/healthz`. State in `NAPPLE_ACCOUNT_DATA_DIR` (`/var/lib/napple/account`). |
| | `NAPPLE_MAIL_PROVIDER` / `NAPPLE_MAIL_FROM` / `NAPPLE_SES_REGION` / `NAPPLE_SES_CONFIG_SET` | `ses` / `no-reply@napple.lat` / the stack region / `napple-transactional` | confirmation and reset codes through SES v2 with the instance role (no keys). SES is in the sandbox until production access is granted: it only delivers to verified recipients. |
| | `NAPPLE_PHOTO_BUCKET` / `NAPPLE_ACCOUNT_S3_BUCKET` | `napple-photos-<account>-<region>` / `napple-saves-<account>-<region>` | private S3 buckets (profile photos / cloud-save blobs), written by the instance role |
| | `NAPPLE_ADMIN_EMAILS` | empty | comma list of admin accounts (optional repository variable is not wired: set it in `/opt/napple/.env` if needed) |
| `napple-social` | `NAPPLE_SOCIAL_BIND` | `0.0.0.0:8083` | `/social/v1/*` (+ WebSocket) and `/healthz`; state in `NAPPLE_SOCIAL_DATA_DIR` (`/var/lib/napple/social`); verifies account JWTs against `NAPPLE_ACCOUNT_URL` (`http://account:8082` inside the compose network) |
| `napple-online` (added) | `NAPPLE_ACCOUNT_URL`, `NAPPLE_SOCIAL_URL` | `http://account:8082`, `http://social:8083` | JWKS and encounter/stat reporting over the compose network |
| `napple-room serve` | `NAPPLE_ROOM_LISTEN` | `0.0.0.0:31415` | room protocol, TCP (31415/udp is opened too) |
| | `NAPPLE_ROOM_HTTP` | `0.0.0.0:8081` | plain-HTTP `GET /healthz` -> 200 `ok`, `GET /rooms` (JSON); served as `/room/healthz`, `/room/rooms` |
| | `NAPPLE_ROOM_DATA` | `/var/lib/napple/room` | accepted, unused (the room server keeps no state); the other `NAPPLE_ROOM_*` limits keep their defaults |

`napple-online` also accepts `NAPPLE_ONLINE_NEX_AUTH` and `NAPPLE_ONLINE_NEX_SECURE` and logs them as unused: NEX itself needs no UDP port.
The NAT-check ports are published by Docker (DNAT keeps the client's address and port; a reply from another published port leaves with that
port, which the check relies on). They are open on IPv4 and IPv6 in the security group; the VPC itself is IPv4-only today. Both are built with `cargo build --release --locked -p <crate> --bin <crate>` from the workspace root on Linux (arm64 and amd64). They need no
system library beyond libc (no OpenSSL), run as uid 10001 in a read-only container, log to stdout/stderr and exit on SIGTERM.
Clients use `NAPPLE_ONLINE_SERVER=https://pretend.napple.lat` and `NAPPLE_ROOM=pretend.napple.lat:31415`.

## One-time setup

1. **Hosted zone.** In Route 53 > Hosted zones, check that a public zone `napple.lat` exists (a domain registered through Route 53 gets one
   automatically) and that the registrar's name servers are the zone's four NS values. The workflow's summary prints them after each apply.
2. **GitHub Pages domain.**
   - Account (or organisation) Settings > Pages > *Add a domain* `napple.lat`. GitHub shows a TXT record: put its value in the repository variable
     `NAPPLE_PAGES_VERIFICATION_TXT` (the stack creates the record), or add the record by hand, then press *Verify*.
   - Repository Settings > Pages > Custom domain: `www.napple.lat`, then *Enforce HTTPS* once the certificate is issued. With Actions-based Pages
     (`pages.yml`) this setting is what counts; `site/CNAME` documents it and is used if the site is ever published from a branch.
3. **GitHub environment.** Repository Settings > Environments > *New environment* `napple-servers`. Under *Deployment branches and tags*, allow
   only `main` (the OIDC trust accepts this environment, so the branch rule is what keeps other branches out). The secrets and variables below
   can live in the repository or in this environment; the workflow reads both.
4. **AWS credentials**, one of:
   - **OIDC role `napple-deploy` (preferred, no long-lived key).** In IAM > Identity providers, add an OpenID Connect provider
     `https://token.actions.githubusercontent.com` with audience `sts.amazonaws.com`. Create the role `napple-deploy` with the trust policy
     [`infra/aws/github-oidc-trust.json`](../../infra/aws/github-oidc-trust.json) (replace `<ACCOUNT_ID>`). It accepts tokens of `erostalevi/napple`
     whose subject is `repo:erostalevi/napple:environment:napple-servers` (every AWS job of the workflow) or `repo:erostalevi/napple:ref:refs/heads/main`, each also as `erostalevi/Napple`: the token carries
     the repository name in its current casing (the repository was renamed `Napple`), and the subject match is case-sensitive.
     Attach [`infra/aws/deploy-policy.json`](../../infra/aws/deploy-policy.json) as an inline policy (least privilege; the live account uses
     `PowerUserAccess` plus an inline IAM policy for `role/napple-*` and `instance-profile/napple-*`, which also works, and this file is the hardening step): it manages only `role/napple-*` and
     `instance-profile/napple-*`, never `napple-deploy` itself (explicit deny). Add the role ARN as the **secret** `AWS_DEPLOY_ROLE_ARN`.
   - **Access keys (fallback).** An IAM user with the same policy and an access key; secrets `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY`.
     The workflow uses the role when `AWS_DEPLOY_ROLE_ARN` is set, else the keys.
   ```bash
   aws iam create-open-id-connect-provider --url https://token.actions.githubusercontent.com --client-id-list sts.amazonaws.com
   ```
   ```bash
   aws iam create-role --role-name napple-deploy --assume-role-policy-document file://trust.json
   ```
   ```bash
   aws iam put-role-policy --role-name napple-deploy --policy-name napple-deploy --policy-document file://infra/aws/deploy-policy.json
   ```
   (`trust.json` = `infra/aws/github-oidc-trust.json` with your account id filled in.)
5. **Variables** (Settings > Secrets and variables > Actions > Variables, or the environment's variables; `vars.*`, not secrets):
   | Variable | Default | Meaning |
   |---|---|---|
   | `AWS_REGION` | `us-east-1` | region of the state, the servers and ECR |
   | `NAPPLE_DOMAIN` | `napple.lat` | the domain whose zone holds the records |
   | `NAPPLE_ZONE_ID` | empty | Route 53 hosted zone id; set = the zone is looked up by id, empty = by `NAPPLE_DOMAIN` |
   | `NAPPLE_INSTANCE_TYPE` | `t4g.micro` | optional; `t4g.small` (2 GB) if memory runs short |
   | `NAPPLE_ARCHITECTURE` | `arm64` | optional; `x86_64` for t3/m6i types |
   | `NAPPLE_PAGES_VERIFICATION_TXT` | empty | optional; the GitHub Pages domain verification value (step 2) |
   | `NAPPLE_DNS_ALLOW_OVERWRITE` | `true` | optional; `false` = fail instead of taking over records made by hand |
6. **Bootstrap the state**: Actions > deploy-servers > *Run workflow* on `main`, mode `bootstrap`. This creates the S3 state bucket
   `napple-tfstate-<account>-<region>` (versioned, encrypted, private, TLS only; locking uses S3 lock files, so there is no DynamoDB table), then moves its own
   state into the bucket. Running it again is safe.
7. **First deploy**: *Run workflow* with mode `plan` to read the plan, then mode `apply` (or push to `main`). The first apply takes about 10 minutes:
   the instance boots, the images are built on an arm64 runner, Caddy gets its certificate once `pretend.napple.lat` resolves, and the smoke test
   retries for up to 5 minutes. This works before the server crates exist: see [stand-in servers](#stand-in-servers).

### Stand-in servers
While any of `crates/napple-online`, `napple-room`, `napple-account` or `napple-social` is not in the deployed commit, the `images` job builds `infra/docker/stub.Dockerfile`
(`infra/local/stub-server.py` on `python:3.13-alpine`, about 25 MB) into that repository instead, with a notice in the run summary. The stand-in
listens on the same ports and env vars as the real server, answers `/healthz` (`ok stub online`, `ok stub room`) and echoes TCP on the room port, so DNS,
TLS, the SSM roll and the smoke test are proven end to end. It holds no state and speaks no game protocol. The first push to `main` that brings
a crate (the workflow triggers on `crates/napple-online/**` and `crates/napple-room/**`) builds the real image and rolls it in its place.

## What a deploy does

| Job | What |
|---|---|
| guard | Resolves the variables, looks for `AWS_DEPLOY_ROLE_ARN` or both access-key secrets, Missing credentials: a notice in the run summary and every other job is skipped (the run is green). `bootstrap`, `apply` and `destroy` only run on `main`; `destroy` also needs the input `confirm` = `destroy`. |
| bootstrap | mode `bootstrap` only: `infra/scripts/bootstrap.sh apply`. |
| infra | `tofu fmt -check`, `validate`, the offline mocked plan tests, then `infra/scripts/tofu-servers.sh plan|apply|destroy` against the S3 state. |
| images | One native build per image (`ubuntu-24.04-arm` for arm64), pushed to ECR as `<repo>:<commit sha>` (immutable tags; a commit already pushed is skipped). |
| roll | `infra/scripts/roll.sh`: through SSM `send-command`, writes `compose.yml`, `compose.aws.yml`, `Caddyfile` and `.env` into `/opt/napple` on the instance, `docker compose pull && up -d`, prunes week-old images. Then `infra/scripts/smoke.sh https://pretend.<domain>` checks `/healthz`, `/room/healthz`, `/account/healthz` and `/social/healthz`. |

Every job runs in the environment `napple-servers`. The workflow has one concurrency group and never cancels a running deploy.

## Rollback and destroy

- **Roll back the servers**: *Run workflow*, mode `apply`, `image_tag` = the commit SHA of a good deploy (ECR keeps the newest 3 images per
  repository, so the rollback window is the last 3 deploys). No build happens; the instance is rolled to those images.
- **Roll back infrastructure**: revert the commit on `main`; the push re-applies the previous configuration. The state bucket keeps 90 days of state
  versions in case a state itself has to be restored.
- **Replace the instance** (new AMI, broken host): from a machine with the deploy credentials, run
  `tofu apply -replace=aws_instance.servers` in `infra/terraform` (same `-backend-config` flags as `infra/scripts/tofu-servers.sh`), then run the
  workflow with mode `apply` so the new instance gets the containers. Accounts, room state and certificates are on the separate data volume and survive.
- **Destroy the servers**: *Run workflow* on `main`, mode `destroy`, `confirm` = `destroy`. It removes the instance, the data volume (so the
  accounts go with it), the Elastic IP, the ECR repositories and their images, the VPC, the `napple-servers-instance` role and profile, the log group
  and the `pretend` record. It **keeps** the hosted zone (never managed by the stack) and the GitHub Pages records (apex, `www`, wildcard,
  verification TXT), so the landing page stays up. A later `apply` builds the servers again. The bootstrap stack (state bucket and lock table) is
  not touched; it has `prevent_destroy`: remove those lines and destroy it by hand to delete it on purpose.
- Disable deploys without deleting anything: remove the AWS secrets (the workflow goes back to skipping).

## Costs (us-east-1)

Cost choices: one on-demand `t4g.micro` (no Spot: stability), gp3 volumes at the minimum sizes (8 GB root = the AMI's snapshot size, 1 GB
data), standard CPU credits (a busy burstable instance slows down instead of billing surplus credits), no NAT gateway, no load balancer, no
detailed monitoring, one Elastic IP, ECR keeps 3 images per repository, logs kept 3 days, no DynamoDB (S3 lock files).

| Item | List price per month |
|---|---|
| t4g.micro (2 vCPU burstable, 1 GB), $0.0084/h | about $6.13 |
| EBS gp3, 8 GB root + 1 GB data, $0.08/GB | about $0.72 |
| Public IPv4 address (the Elastic IP), $0.005/h | about $3.65 |
| Route 53 hosted zone (+ $0.40 per million queries) | $0.50 |
| ECR (2 repositories x 3 images, well under 1 GB), CloudWatch logs (3 days), S3 state | a few cents |
| Data transfer out | first 100 GB/month free, then about $0.09/GB |
| **Total at list price** | **about $11/month** for a quiet server; traffic is the variable part |

**With the free tier.** `aws ec2 describe-instance-types --filters Name=free-tier-eligible,Values=true` lists `t4g.micro` (and `t4g.small`) for
this account, so the instance hours fall under the account's free tier: on the current free plan they are paid from the free credits, and on the
legacy 12-month free tier 750 instance hours, 750 hours of public IPv4, 30 GB of EBS, 500 MB of ECR storage and 5 GB of log ingestion are free
each month. In both cases the only charge that is certain is the hosted zone, $0.50/month. Which program applies is in Billing > Free Tier.
Leaving it running after the free period costs the list price above.

`t4g.micro` has 1 GB of memory (plus the 1 GB swap file the user data adds): enough for Caddy, the stand-ins and two small Rust servers. If the
real servers need more, set `NAPPLE_INSTANCE_TYPE=t4g.small` (about $12.3/month at list price, also free-tier eligible here).

## Security notes

- **No game data on the server, ever.** The images contain only the two server binaries (the Dockerfiles copy nothing but `Cargo.toml`,
  `Cargo.lock` and `crates/` into the build stage, and only the binary into the final image). No game files, keys, firmware or title data are
  uploaded, and the servers never ask clients for any. `app.napple.lat` (hosted uploads) was rejected for this reason.
- Credentials live only in GitHub secrets (or are short-lived, through OIDC). Nothing in `infra/` holds a credential; the instance uses its role.
- The OIDC trust policy only accepts this repository's environment `napple-servers` (limited to `main` by its branch rule) and `main`. The deploy
  policy is scoped to `napple-*` names where AWS allows it and denies any change to `napple-deploy` itself.
- Instance: no SSH port (Session Manager), IMDSv2 only, encrypted volumes, unattended security updates. Containers: the servers run as uid 10001; Caddy runs as
  root in its container with only `NET_BIND_SERVICE` kept, read-only root filesystems, all other capabilities dropped, `no-new-privileges`.
- The internet-facing parsers are napple-online's HTTP/WebSocket/PRUDP Lite code (behind Caddy) and the room protocol (direct TCP): they take
  untrusted input, so they want fuzzing and per-peer rate limits (the server crates' job; napple-room has rate limits built in).
- Verify `napple.lat` in GitHub before the wildcard goes live (see [DNS records](#dns-records)).

## Local test stack

No AWS and no Docker needed:

```bash
infra/local/run.sh
```

It builds the four crates (debug; `--release` for release), starts them on 127.0.0.1 with a throwaway data dir under `$TMPDIR/napple-local-stack`
(account 18082, social 18083, online 18080, room 31415/18081; the account service uses the dev mailer, so sign-in codes appear in `logs/account.log`),
waits for `/healthz`, and prints the env for a Napple runtime (`NAPPLE_NET=host`, `NAPPLE_ONLINE_SERVER=http://127.0.0.1:18080`,
`NAPPLE_ACCOUNT_URL=http://127.0.0.1:18082`, `NAPPLE_SOCIAL_URL=http://127.0.0.1:18083`, `NAPPLE_ROOM=127.0.0.1:31415`). `infra/local/run.sh stop` stops all four and deletes the data (`KEEP_DATA=1` keeps it); `status`, `logs` and `env` do what
they say. `--bin-dir DIR` runs prebuilt binaries; `--stub` runs `infra/local/stub-server.py`, a stand-in that answers `/healthz` and echoes TCP on the room port,
for testing the plumbing without the crates.

With Docker installed, the production stack runs locally too (Caddy uses its internal CA for `localhost`):

```bash
cd infra/docker && NAPPLE_DOMAIN=localhost docker compose -f compose.yml -f compose.build.yml up --build
```

## Checks without AWS

```bash
cd infra/terraform && tofu init -backend=false && tofu validate && tofu test
```
```bash
cd infra/terraform/bootstrap && tofu init -backend=false && tofu validate && tofu test
```
```bash
actionlint .github/workflows/deploy-servers.yml && shellcheck -x infra/scripts/*.sh infra/local/run.sh
```

The `tofu test` files plan both stacks against a mocked AWS provider: no credentials, no AWS call. The deploy scripts refuse to run outside
GitHub Actions unless `NAPPLE_DEPLOY_ALLOW_LOCAL=1` is set.

## Troubleshooting

| Symptom | Look at |
|---|---|
| Run is green but did nothing | The run summary: the guard's notice says which secret or crate is missing. |
| A `plan` from another branch fails before the guard | The environment `napple-servers` only allows `main`: run it on `main`, or allow the branch in the environment. |
| `Not authorized to perform sts:AssumeRoleWithWebIdentity` | The trust policy's subjects: the jobs present `repo:erostalevi/napple:environment:napple-servers`. |
| `State bucket ... not found` | Run mode `bootstrap` once. |
| Apply fails with "record already exists" | `NAPPLE_DNS_ALLOW_OVERWRITE` is `false` and a record was made by hand: delete it, or set the variable back to `true`. |
| The smoke test prints `ok stub online` | The stand-in is deployed: the crate is not on `main` yet ([stand-in servers](#stand-in-servers)). |
| Roll fails with `/opt/napple or docker compose missing` | First boot failed: the error prints the end of `/var/log/napple-bootstrap.log`. A slow first boot is waited for (cloud-init, up to 10 min). |
| Roll waits for SSM forever | The instance has no route out (check the route table) or the SSM agent is down: EC2 > the instance > Get system log; `/var/log/napple-bootstrap.log` from a Session Manager shell. |
| Smoke test fails on a first deploy | DNS not delegated yet (check the registrar's name servers) or the ACME challenge failed: CloudWatch `/napple/servers`, the caddy stream. |
| `docker compose` errors on the instance | `aws ssm start-session --target <instance id>`, then `cd /opt/napple && sudo docker compose -f compose.yml -f compose.aws.yml logs`. |
