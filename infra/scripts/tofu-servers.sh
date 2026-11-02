#!/usr/bin/env bash
# Plan, apply or destroy the servers stack (infra/terraform) against the S3 state from the bootstrap stack.
#   tofu-servers.sh plan|apply|destroy
# destroy removes the servers (instance, data volume, EIP, ECR repositories with their images, VPC, IAM role, log group, the pretend record) and
# keeps the hosted zone (never managed here) and the GitHub Pages records, so the landing page stays up. The bootstrap stack is not touched.
# Writes the stack outputs as step outputs (instance_id, public_ip, server_fqdn, log_group, ecr_online, ecr_room) after an apply,
# or after a plan when a state already exists.
# shellcheck source=common.sh source-path=SCRIPTDIR
source "$(dirname "$0")/common.sh"
require_ci
mode="${1:-plan}"
case "$mode" in plan|apply|destroy) ;; *) echo "usage: $0 plan|apply|destroy" >&2; exit 2 ;; esac

state_names
if ! aws s3api head-bucket --bucket "$TF_STATE_BUCKET" --expected-bucket-owner "$ACCOUNT_ID" 2>/dev/null; then
  summary "::error::State bucket \`$TF_STATE_BUCKET\` not found: run the workflow once with mode=bootstrap (docs/guides/deploy-servers.md)."
  exit 1
fi

cd "$INFRA_DIR/terraform"
export TF_VAR_region="$AWS_REGION"
mapfile -t backend < <(backend_args servers/terraform.tfstate)
"$TF" init -input=false "${backend[@]}"

if [ "$mode" = destroy ]; then
  keep=(-exclude=aws_route53_record.apex_a -exclude=aws_route53_record.apex_aaaa -exclude=aws_route53_record.www
        -exclude=aws_route53_record.wildcard -exclude=aws_route53_record.pages_verification)
  # force_delete is read from the state at destroy time: record it first, so repositories holding images can go.
  "$TF" apply -input=false -auto-approve -target=aws_ecr_repository.image -var ecr_force_delete=true
  "$TF" destroy -input=false -auto-approve -var ecr_force_delete=true "${keep[@]}"
  summary "Destroyed the servers stack (hosted zone and GitHub Pages records kept). Accounts on the data volume are gone."
  exit 0
fi

"$TF" plan -input=false -out=tfplan
if [ "$mode" = apply ]; then
  "$TF" apply -input=false tfplan
fi

if "$TF" output -json > outputs.json 2>/dev/null && [ "$(jq 'has("instance_id")' outputs.json)" = true ]; then
  gh_output instance_id "$(jq -r .instance_id.value outputs.json)"
  gh_output public_ip "$(jq -r .public_ip.value outputs.json)"
  gh_output server_fqdn "$(jq -r .server_fqdn.value outputs.json)"
  gh_output log_group "$(jq -r .log_group.value outputs.json)"
  gh_output ecr_online "$(jq -r '.ecr_repository_urls.value["napple-online"]' outputs.json)"
  gh_output ecr_room "$(jq -r '.ecr_repository_urls.value["napple-room"]' outputs.json)"
  summary "Servers: \`$(jq -r .server_fqdn.value outputs.json)\` -> \`$(jq -r .public_ip.value outputs.json)\` (instance \`$(jq -r .instance_id.value outputs.json)\`)."
  summary "Zone name servers (the registrar must list these): $(jq -r '.zone_name_servers.value | join(", ")' outputs.json)"
fi
