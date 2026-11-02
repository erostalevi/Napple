# infra: deployment of the Napple game servers

How to use it: [docs/guides/deploy-servers.md](../docs/guides/deploy-servers.md). Deploys run from `.github/workflows/deploy-servers.yml` and do
nothing until AWS credentials are in the repository secrets.

| Path | What |
|---|---|
| `docker/napple-online.Dockerfile`, `docker/napple-room.Dockerfile` | Multi-stage Rust builds (context = repo root, allowlisted by the `.dockerignore` next to each), non-root runtime images with a `/healthz` health check |
| `docker/compose.yml` | The stack: `online`, `room`, `caddy` (TLS for `pretend.napple.lat`); `compose.aws.yml` = EBS bind mounts + CloudWatch logs, `compose.build.yml` = local builds |
| `docker/Caddyfile` | `/room/*` -> napple-room status API, everything else -> napple-online HTTP API |
| `terraform/bootstrap/` | One-time stack: S3 state bucket + DynamoDB lock table |
| `terraform/` | Servers stack: VPC, security group, EC2 (AL2023, Docker), EIP, data volume, IAM role, ECR, log group, Route 53 records in the existing zone |
| `terraform/**/tests/*.tftest.hcl` | Offline plan tests with a mocked AWS provider (`tofu test`) |
| `aws/` | The GitHub OIDC trust policy and the deploy policy to create by hand once |
| `scripts/` | What the workflow runs: `bootstrap.sh`, `tofu-servers.sh`, `roll.sh` (SSM), `smoke.sh` |
| `local/run.sh` | Native local stack on 127.0.0.1 (no Docker); `local/stub-server.py` stands in for the binaries |
