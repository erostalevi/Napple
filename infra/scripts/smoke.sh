#!/usr/bin/env bash
# Smoke test of a deployed (or local) stack: online, room, account and social answer /healthz.
#   smoke.sh https://pretend.napple.lat [tries]
# A first deploy waits for DNS and the ACME certificate, so it retries (default 30 x 10 s).
set -uo pipefail
base="${1:?usage: $0 <base url> [tries]}"; tries="${2:-30}"
for path in /healthz /room/healthz /account/healthz /social/v1/healthz; do
  ok=0
  for i in $(seq 1 "$tries"); do
    if body="$(curl -fsS --max-time 10 "$base$path")"; then ok=1; echo "$base$path -> ${body:0:200}"; break; fi
    echo "try $i/$tries: $base$path not healthy yet"; sleep 10
  done
  [ "$ok" = 1 ] || { echo "::error::$base$path never became healthy" >&2; exit 1; }
done
