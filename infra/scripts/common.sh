# Shared by the deploy scripts (sourced, not run). Names of the state bucket and lock table, derived the same way as the bootstrap stack.
# Needs AWS credentials in the environment (the workflow's configure-aws-credentials step); never run from a laptop by accident: see require_ci.
set -euo pipefail

INFRA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export INFRA_DIR
export AWS_REGION="${AWS_REGION:-us-east-1}"
export AWS_DEFAULT_REGION="$AWS_REGION"
TF="${TF:-tofu}"

require_ci() {
  if [ "${GITHUB_ACTIONS:-}" != "true" ] && [ "${NAPPLE_DEPLOY_ALLOW_LOCAL:-}" != "1" ]; then
    echo "refusing to run outside GitHub Actions (set NAPPLE_DEPLOY_ALLOW_LOCAL=1 to run it by hand with your own AWS credentials)" >&2
    exit 2
  fi
}

account_id() { aws sts get-caller-identity --query Account --output text; }

state_names() {
  ACCOUNT_ID="$(account_id)"
  TF_STATE_BUCKET="${TF_STATE_BUCKET:-napple-tfstate-$ACCOUNT_ID-$AWS_REGION}"
  TF_LOCK_TABLE="${TF_LOCK_TABLE:-napple-tflock}"
  export ACCOUNT_ID TF_STATE_BUCKET TF_LOCK_TABLE
}

# backend_args <key>: the -backend-config flags of the partial S3 backend.
backend_args() {
  printf '%s\n' "-backend-config=bucket=$TF_STATE_BUCKET" "-backend-config=key=$1" "-backend-config=region=$AWS_REGION" \
    "-backend-config=dynamodb_table=$TF_LOCK_TABLE" "-backend-config=encrypt=true"
}

# gh_output <name> <value>: a step output when running in Actions, a line on stdout otherwise.
gh_output() {
  if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "$1=$2" >> "$GITHUB_OUTPUT"; else echo "$1=$2"; fi
}

summary() {
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then echo "$*" >> "$GITHUB_STEP_SUMMARY"; fi
  echo "$*"
}
