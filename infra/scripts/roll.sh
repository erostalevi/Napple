#!/usr/bin/env bash
# Roll the servers instance to the given images through SSM send-command (no SSH): write compose.yml, compose.aws.yml, Caddyfile and .env
# into /opt/napple, log in to ECR, `docker compose pull && up -d`, prune old images.
#   roll.sh <instance id> <online image ref> <room image ref> <public ip> <domain> <log group>
# Rollback = run it again with older image refs (workflow_dispatch input image_tag).
# shellcheck source=common.sh source-path=SCRIPTDIR
source "$(dirname "$0")/common.sh"
require_ci
[ $# -eq 6 ] || { echo "usage: $0 <instance id> <online image> <room image> <public ip> <domain> <log group>" >&2; exit 2; }
instance="$1" online_image="$2" room_image="$3" public_ip="$4" domain="$5" log_group="$6"
registry="${online_image%%/*}"

bundle="$(tar -C "$INFRA_DIR/docker" -czf - compose.yml compose.aws.yml Caddyfile | base64 | tr -d '\n')"
env_file="$(printf '%s\n' \
  "NAPPLE_ONLINE_IMAGE=$online_image" "NAPPLE_ROOM_IMAGE=$room_image" "NAPPLE_DOMAIN=$domain" \
  "NAPPLE_PUBLIC_IP=$public_ip" "AWS_REGION=$AWS_REGION" "NAPPLE_LOG_GROUP=$log_group" | base64 | tr -d '\n')"

script=$(cat <<SH
set -euo pipefail
cd /opt/napple
echo '$bundle' | base64 -d | tar -xzf -
echo '$env_file' | base64 -d > .env.new && mv .env.new .env && chmod 600 .env
aws ecr get-login-password --region $AWS_REGION | docker login --username AWS --password-stdin $registry
docker compose -f compose.yml -f compose.aws.yml pull
docker compose -f compose.yml -f compose.aws.yml up -d --remove-orphans
docker image prune -af --filter until=168h >/dev/null || true
sleep 5
docker compose -f compose.yml -f compose.aws.yml ps
SH
)

# Wait for the instance to register with SSM (a fresh instance takes a minute or two after boot).
for _ in $(seq 1 40); do
  state="$(aws ssm describe-instance-information --filters "Key=InstanceIds,Values=$instance" --query 'InstanceInformationList[0].PingStatus' --output text 2>/dev/null || true)"
  [ "$state" = Online ] && break
  echo "waiting for SSM on $instance ($state)"; sleep 15
done
[ "$state" = Online ] || { echo "::error::instance $instance never came online in SSM" >&2; exit 1; }

params="$(jq -cn --arg s "$script" '{commands: [$s], executionTimeout: ["900"]}')"
cmd_id="$(aws ssm send-command --instance-ids "$instance" --document-name AWS-RunShellScript \
  --comment "napple deploy ${GITHUB_SHA:-manual}" --parameters "$params" --query Command.CommandId --output text)"
echo "SSM command $cmd_id"

status=Pending
for _ in $(seq 1 90); do
  sleep 10
  status="$(aws ssm get-command-invocation --command-id "$cmd_id" --instance-id "$instance" --query Status --output text 2>/dev/null || echo Pending)"
  case "$status" in Pending|InProgress|Delayed) continue ;; *) break ;; esac
done
aws ssm get-command-invocation --command-id "$cmd_id" --instance-id "$instance" --query '[StandardOutputContent,StandardErrorContent]' --output text || true
[ "$status" = Success ] || { echo "::error::deploy command ended $status" >&2; exit 1; }
summary "Rolled \`$instance\` to \`$online_image\` and \`$room_image\`."
