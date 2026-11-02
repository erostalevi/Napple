# napple-infra (2026-11-02, batch r7): deploy the game servers to pretend.napple.lat, DNS for napple.lat, local test stack

Branch `claude/napple-infra-2026-11-02-r7` (Mac session, base `91c5b2e`). Guide: [deploy-servers](../../guides/deploy-servers.md).

## What changed
- `infra/docker/`: multi-stage Dockerfiles for `napple-online` and `napple-room` (build context allowlisted to the Rust workspace, non-root
  Debian slim runtime, `/healthz` health checks), `compose.yml` with Caddy terminating TLS for `pretend.napple.lat` (`/room/*` -> room, the rest ->
  online), the NEX UDP ports and the room port published directly; `compose.aws.yml` (EBS bind mounts, CloudWatch logs) and `compose.build.yml`.
- `infra/terraform/bootstrap/`: S3 state bucket + DynamoDB lock table, created once by the workflow, which then migrates the stack's own state into the bucket.
- `infra/terraform/`: own VPC, public subnet, security group (80/443, 443/udp, 60000-60010/udp, 31415 tcp+udp, no SSH), t4g.small on AL2023
  with Docker + Compose from user data, Elastic IP, a separate encrypted data volume at `/var/lib/napple`, instance role (SSM, ECR pull, logs),
  two immutable-tag ECR repositories, the log group, and the Route 53 records in the existing zone: `pretend` A, apex A/AAAA to GitHub Pages,
  `www` and `*` CNAME to `<owner>.github.io`, an optional Pages verification TXT.
- `.github/workflows/deploy-servers.yml`: a guard job (secrets present? crates present? `main` for apply) that skips cleanly, bootstrap, plan/apply,
  native arm64 image builds to ECR, the SSM roll and the smoke test. Scripts in `infra/scripts/`; trust and deploy policies in `infra/aws/`.
- `infra/local/run.sh`: native local stack on 127.0.0.1 (no Docker), `--stub` / `--bin-dir` for testing without the crates.
- `site/CNAME` = `www.napple.lat`.
- Names per the orchestrator's contract: secret `AWS_DEPLOY_ROLE_ARN` (role `napple-deploy`) + access-key fallback, variables `AWS_REGION`,
  `NAPPLE_DOMAIN`, `NAPPLE_ZONE_ID`, environment `napple-servers`, modes `bootstrap`/`plan`/`apply`/`destroy` (destroy keeps the zone and the Pages records).

## Status
- **tested**: both stacks `tofu fmt -check`, `validate` and `tofu test` (8 mocked plan runs, no AWS call); actionlint and shellcheck clean; the
  workflow's guard script run for 7 secret/branch/mode combinations; `docker compose config` of the three compose combinations; `tools/check_docs.py`.
- **verified (plumbing only)**: `infra/local/run.sh` with the stand-in server: start, `/healthz`, UDP echo on the NEX ports, TCP on the room port,
  refusal of a second start, clean stop, failure cleanup.
- **untested**: the `destroy` path, the Docker image builds (no Docker on this Mac), any real AWS apply, the SSM roll, Caddy's certificate, the real server
  binaries (the crates were not committed on their branches at hand-off).
