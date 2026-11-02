#!/usr/bin/env bash
# Plan or apply the servers stack (infra/terraform) against the S3 state from the bootstrap stack.
#   tofu-servers.sh plan|apply
# Writes the stack outputs as step outputs (instance_id, public_ip, server_fqdn, log_group, ecr_online, ecr_room) after an apply,
# or after a plan when a state already exists.
source "$(dirname "$0")/common.sh"
require_ci
mode="${1:-plan}"
case "$mode" in plan|apply) ;; *) echo "usage: $0 plan|apply" >&2; exit 2 ;; esac

state_names
if ! aws s3api head-bucket --bucket "$TF_STATE_BUCKET" --expected-bucket-owner "$ACCOUNT_ID" 2>/dev/null; then
  summary "::error::State bucket \`$TF_STATE_BUCKET\` not found: run the workflow once with mode=bootstrap (docs/guides/deploy-servers.md)."
  exit 1
fi

cd "$INFRA_DIR/terraform"
export TF_VAR_region="$AWS_REGION"
mapfile -t backend < <(backend_args servers/terraform.tfstate)
"$TF" init -input=false "${backend[@]}"
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
