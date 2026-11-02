#!/usr/bin/env bash
# One-time (idempotent) creation of the OpenTofu state bucket: infra/terraform/bootstrap (locking = S3 lock files, no DynamoDB).
#   bootstrap.sh plan|apply
# First run: the stack is applied with a local state, then the state is migrated into the bucket it created (key bootstrap/terraform.tfstate).
# Later runs: the state is read from the bucket and the stack is planned/applied like any other.
# shellcheck source=common.sh source-path=SCRIPTDIR
source "$(dirname "$0")/common.sh"
require_ci
mode="${1:-plan}"
case "$mode" in plan|apply) ;; *) echo "usage: $0 plan|apply" >&2; exit 2 ;; esac

state_names
cd "$INFRA_DIR/terraform/bootstrap"
export TF_VAR_region="$AWS_REGION" TF_VAR_state_bucket_name="$TF_STATE_BUCKET"

write_backend_override() {
  cat > backend_override.tf <<HCL
terraform {
  backend "s3" {
    bucket         = "$TF_STATE_BUCKET"
    key            = "bootstrap/terraform.tfstate"
    region         = "$AWS_REGION"
    use_lockfile   = true
    encrypt        = true
  }
}
HCL
}

if aws s3api head-bucket --bucket "$TF_STATE_BUCKET" --expected-bucket-owner "$ACCOUNT_ID" 2>/dev/null; then
  summary "State bucket \`$TF_STATE_BUCKET\` exists: bootstrap stack read from it."
  write_backend_override
  "$TF" init -input=false
  if [ "$mode" = apply ]; then "$TF" apply -input=false -auto-approve; else "$TF" plan -input=false; fi
else
  summary "State bucket \`$TF_STATE_BUCKET\` does not exist yet."
  "$TF" init -input=false
  if [ "$mode" = plan ]; then "$TF" plan -input=false; exit 0; fi
  "$TF" apply -input=false -auto-approve
  write_backend_override
  "$TF" init -input=false -migrate-state -force-copy
  rm -f terraform.tfstate terraform.tfstate.backup
  summary "Bootstrapped: bucket \`$TF_STATE_BUCKET\` ($AWS_REGION, S3 lock files); state migrated into the bucket."
fi
