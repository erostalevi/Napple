# napple-infra (2026-11-02, batch r7): deploy the game servers to pretend.napple.lat, DNS for napple.lat, local test stack

Branch `claude/napple-infra-2026-11-02-r7` (Mac session, base `91c5b2e`). Guide: [deploy-servers](../../guides/deploy-servers.md).

## What changed
- `infra/docker/`: multi-stage Dockerfiles for `napple-online` and `napple-room` (build context allowlisted to the Rust workspace, non-root
  Debian slim runtime, `/healthz` health checks), `compose.yml` with Caddy terminating TLS for `pretend.napple.lat` (`/room/*` -> room, the rest ->
  online, NEX as a WebSocket included), the room port published directly; `compose.aws.yml` (EBS bind mounts, CloudWatch logs) and `compose.build.yml`.
- `infra/terraform/bootstrap/`: the S3 state bucket (locking with S3 lock files, no DynamoDB), created once by the workflow, which then migrates
  the stack's own state into the bucket.
- `infra/terraform/`: own VPC, public subnet, security group (80/443, 443/udp, 31415 tcp+udp, no SSH), t4g.micro on AL2023
  with Docker + Compose from user data (8 GB root, standard CPU credits), Elastic IP, a 1 GB encrypted data volume at `/var/lib/napple`, instance role (SSM, ECR pull, logs),
  two immutable-tag ECR repositories, the log group, and the Route 53 records in the existing zone: `pretend` A, apex A/AAAA to GitHub Pages,
  `www` and `*` CNAME to `<owner>.github.io`, an optional Pages verification TXT.
- `.github/workflows/deploy-servers.yml`: a guard job (secrets present? crates present? `main` for apply) that skips cleanly, bootstrap, plan/apply,
  native arm64 image builds to ECR, the SSM roll and the smoke test. Scripts in `infra/scripts/`; trust and deploy policies in `infra/aws/`.
- `infra/local/run.sh`: native local stack on 127.0.0.1 (no Docker), `--stub` / `--bin-dir` for testing without the crates.
- Stand-in image `infra/docker/stub.Dockerfile`: deployed in place of a server whose crate is not on `main` yet, so the whole path can go live first.
- Cost: ECR keeps 3 images, logs 3 days, about $11/month at list price with the instance under the free tier.
- `site/CNAME` = `www.napple.lat`.
- Names per the orchestrator's contract: secret `AWS_DEPLOY_ROLE_ARN` (role `napple-deploy`) + access-key fallback, variables `AWS_REGION`,
  `NAPPLE_DOMAIN`, `NAPPLE_ZONE_ID`, environment `napple-servers`, modes `bootstrap`/`plan`/`apply`/`destroy` (destroy keeps the zone and the Pages records).

## Status
- **tested**: both stacks `tofu fmt -check`, `validate` and `tofu test` (10 mocked plan runs, no AWS call); actionlint and shellcheck clean; the
  workflow's guard and image-source steps run for 8 + 3 cases; the stand-in's entrypoint and health check run natively; `docker compose config` of the three compose combinations; `tools/check_docs.py`.
- **verified (plumbing only)**: `infra/local/run.sh` with the stand-in server: start, `/healthz`, TCP on the room port,
  refusal of a second start, clean stop, failure cleanup.
- **verified (real servers)**: `infra/local/run.sh` builds and starts the merged napple-online (f7c5b68) and napple-room (93da095) crates;
  both `/healthz` answer `ok`, `/rooms` lists none, stop is clean. napple-online needs no UDP (NEX is a WebSocket on its HTTP port), so no NEX ports are opened.
- **untested**: the `destroy` path, the Docker image builds (the stand-in's too) (no Docker on this Mac), any real AWS apply, the SSM roll, Caddy's certificate, the real server images on Linux.
