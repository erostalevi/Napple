#!/usr/bin/env bash
# Roll the servers instance to the given images through SSM send-command (no SSH): write compose.yml, compose.aws.yml, Caddyfile and .env
# into /opt/napple, log in to ECR, `docker compose pull && up -d`, prune old images.
#   roll.sh <instance id> <online image ref> <room image ref> <public ip> <domain> <log group> <account image ref> <social image ref> <photo bucket> <saves bucket>
# Rollback = run it again with older image refs (workflow_dispatch input image_tag).
# shellcheck source=common.sh source-path=SCRIPTDIR
source "$(dirname "$0")/common.sh"
require_ci
[ $# -eq 10 ] || { echo "usage: $0 <instance id> <online image> <room image> <public ip> <domain> <log group> <account image> <social image> <photo bucket> <saves bucket>" >&2; exit 2; }
instance="$1" online_image="$2" room_image="$3" public_ip="$4" domain="$5" log_group="$6"
account_image="$7" social_image="$8" photo_bucket="$9" saves_bucket="${10}"
registry="${online_image%%/*}"

bundle="$(tar -C "$INFRA_DIR/docker" -czf - compose.yml compose.aws.yml Caddyfile | base64 | tr -d '\n')"
env_file="$(printf '%s\n' \
  "NAPPLE_ONLINE_IMAGE=$online_image" "NAPPLE_ROOM_IMAGE=$room_image" "NAPPLE_DOMAIN=$domain" \
  "NAPPLE_PUBLIC_IP=$public_ip" "AWS_REGION=$AWS_REGION" "NAPPLE_LOG_GROUP=$log_group" \
  "NAPPLE_ACCOUNT_IMAGE=$account_image" "NAPPLE_SOCIAL_IMAGE=$social_image" \
  "NAPPLE_PHOTO_BUCKET=$photo_bucket" "NAPPLE_SAVES_BUCKET=$saves_bucket" \
  "NAPPLE_MAIL_PROVIDER=ses" "NAPPLE_MAIL_FROM=no-reply@napple.lat" "NAPPLE_SES_CONFIG_SET=napple-transactional" | base64 | tr -d '\n')"

# The command runs on the instance (escaped \$ = expanded there). On a fresh instance it can arrive while the user data is still installing Docker
# and Compose, so it first waits for cloud-init (at most 10 minutes) and checks /opt/napple and `docker compose` before touching anything.
script=$(cat <<SH
set -euo pipefail
timeout 600 cloud-init status --wait >/dev/null 2>&1 || echo "cloud-init: \$(cloud-init status 2>&1 | tr '\n' ' ')"
for _ in \$(seq 1 60); do [ -d /opt/napple ] && docker compose version >/dev/null 2>&1 && break; sleep 5; done
if [ ! -d /opt/napple ] || ! docker compose version; then
  echo "first boot did not finish (/opt/napple or docker compose missing); last lines of /var/log/napple-bootstrap.log:" >&2
  tail -n 30 /var/log/napple-bootstrap.log >&2 || true
  exit 1
fi
cd /opt/napple
# State dirs of the services added after the first boot (uid 10001, like online and room).
install -d -o 10001 -g 10001 -m 0750 /var/lib/napple/account /var/lib/napple/social
echo '$bundle' | base64 -d | tar -xzf -
echo '$env_file' | base64 -d > .env.new
# Service credentials shared between the containers: generated once on the instance, kept in /opt/napple/.service-tokens (0600), never in git or the workflow.
# NAPPLE_INTERNAL_TOKEN is napple-account's and (as NAPPLE_ACCOUNT_SERVICE_TOKEN) napple-social's; NAPPLE_SOCIAL_SERVICE_TOKEN is napple-social's and napple-online's.
if [ ! -s .service-tokens ]; then
  ( umask 077; printf 'NAPPLE_INTERNAL_TOKEN=%s\nNAPPLE_SOCIAL_SERVICE_TOKEN=%s\n' "\$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" "\$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" > .service-tokens )
fi
cat .service-tokens >> .env.new
mv .env.new .env && chmod 600 .env
aws ecr get-login-password --region $AWS_REGION | docker login --username AWS --password-stdin $registry
docker compose -f compose.yml -f compose.aws.yml pull
docker compose -f compose.yml -f compose.aws.yml up -d --remove-orphans
# tar replaces the Caddyfile with a new inode, so the running Caddy keeps reading the old file through its single-file bind mount: recreate it (certificates live in the caddy-data volume).
docker compose -f compose.yml -f compose.aws.yml up -d --force-recreate --no-deps caddy
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

# executionTimeout covers the cloud-init wait (600 s) plus pull/up; the poll below waits a little longer than that.
params="$(jq -cn --arg s "$script" '{commands: [$s], executionTimeout: ["1200"]}')"
cmd_id="$(aws ssm send-command --instance-ids "$instance" --document-name AWS-RunShellScript \
  --comment "napple deploy ${GITHUB_SHA:-manual}" --parameters "$params" --query Command.CommandId --output text)"
echo "SSM command $cmd_id"

status=Pending
for _ in $(seq 1 130); do
  sleep 10
  status="$(aws ssm get-command-invocation --command-id "$cmd_id" --instance-id "$instance" --query Status --output text 2>/dev/null || echo Pending)"
  case "$status" in Pending|InProgress|Delayed) continue ;; *) break ;; esac
done
aws ssm get-command-invocation --command-id "$cmd_id" --instance-id "$instance" --query '[StandardOutputContent,StandardErrorContent]' --output text || true
[ "$status" = Success ] || { echo "::error::deploy command ended $status" >&2; exit 1; }
summary "Rolled \`$instance\` to \`$online_image\`, \`$room_image\`, \`$account_image\` and \`$social_image\`."
