#!/bin/bash
# Local walkthrough of napple-account: sqlite + filesystem blobs in a temp dir, dev mailer, curl.
set -euo pipefail
BIN=${BIN:-target/release/napple-account}
DATA=$(mktemp -d)
export NAPPLE_ACCOUNT_DATA_DIR=$DATA NAPPLE_ACCOUNT_BIND=127.0.0.1:18082 NAPPLE_ACCOUNT_PUBLIC_BASE=http://127.0.0.1:18082/account/v1
"$BIN" > "$DATA/server.log" 2>&1 & PID=$!
trap 'kill $PID 2>/dev/null; wait $PID 2>/dev/null; rm -rf "$DATA"' EXIT
API=$NAPPLE_ACCOUNT_PUBLIC_BASE
for i in $(seq 50); do curl -fsS "$API/healthz" >/dev/null 2>&1 && break; sleep 0.1; done
echo "== healthz";   curl -fsS http://127.0.0.1:18082/healthz
echo "== jwks";      curl -fsS "$API/.well-known/jwks.json"; echo
PUB=$(head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=')
DEV="{\"name\":\"Walk Mac\",\"platform\":\"macos\",\"pubkey\":\"$PUB\"}"
echo "== signup"; curl -fsS -X POST "$API/auth/signup" -H 'content-type: application/json' \
  -d "{\"email\":\"walk@napple.test\",\"password\":\"correct horse battery\",\"nickname\":\"Walker_1\",\"age_ok\":true,\"terms_ok\":true,\"device\":$DEV}"; echo
CODE=$(grep -E '^ +[0-9]{6}$' "$DATA/server.log" | tail -1 | tr -d ' ')
echo "== verify (code from the dev mailer on stdout)"
TOK=$(curl -fsS -X POST "$API/auth/verify" -H 'content-type: application/json' -d "{\"email\":\"walk@napple.test\",\"code\":\"$CODE\"}")
echo "$TOK" | python3 -c 'import json,sys; v=json.load(sys.stdin); print({k: v[k] for k in ("account_id","device_id","key_state","new_account","expires_in")})'
AT=$(echo "$TOK" | python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])')
H="authorization: Bearer $AT"
echo "== me";        curl -fsS "$API/me" -H "$H" | python3 -c 'import json,sys; v=json.load(sys.stdin); print(v["email"], v["plan"], v["storage"]["quota"], v["storage"]["versions_per_title"])'
PROFILE=$(curl -fsS "$API/profiles" -H "$H" | python3 -c 'import json,sys; print(json.load(sys.stdin)["profiles"][0]["profile_id"])')
echo "== save version 1 (title LAS, profile $PROFILE)"
printf 'NAPPLESV\001 pretend ciphertext' > "$DATA/blob"
SIZE=$(wc -c < "$DATA/blob" | tr -d ' '); SHA=$(shasum -a 256 "$DATA/blob" | awk '{print $1}')
C=$(curl -fsS -X POST "$API/saves/01006BB00C6F0000/$PROFILE/versions" -H "$H" -H 'content-type: application/json' -d "{\"base_version\":0,\"size\":$SIZE,\"sha256_ciphertext\":\"$SHA\"}")
URL=$(echo "$C" | python3 -c 'import json,sys; print(json.load(sys.stdin)["upload_url"])')
CK=$(echo "$C" | python3 -c 'import json,sys; print(json.load(sys.stdin)["upload_headers"]["x-amz-checksum-sha256"])')
curl -fsS -X PUT "$URL" -H 'content-type: application/octet-stream' -H "x-amz-checksum-sha256: $CK" --data-binary @"$DATA/blob" -o /dev/null -w "upload HTTP %{http_code}\n"
echo "== commit";    curl -fsS -X POST "$API/saves/01006BB00C6F0000/$PROFILE/versions/1/commit" -H "$H"; echo
echo "== stale base -> 409"; curl -sS -o /dev/null -w "%{http_code}\n" -X POST "$API/saves/01006BB00C6F0000/$PROFILE/versions" -H "$H" -H 'content-type: application/json' -d "{\"base_version\":0,\"size\":1,\"sha256_ciphertext\":\"$SHA\"}"
echo "== second title syncs too (no plans) -> 201"; curl -sS -o /dev/null -w "%{http_code}\n" -X POST "$API/saves/0100000000010000/$PROFILE/versions" -H "$H" -H 'content-type: application/json' -d "{\"base_version\":0,\"size\":1,\"sha256_ciphertext\":\"$SHA\"}"
echo "== reserved name -> 422"; curl -sS -X PATCH "$API/me" -H "$H" -H 'content-type: application/json' -d '{"display_name":"N4PPLE fan"}'; echo
echo "== list";      curl -fsS "$API/saves" -H "$H"; echo
DL=$(curl -fsS "$API/saves/01006BB00C6F0000/$PROFILE/versions/1" -H "$H" | python3 -c 'import json,sys; print(json.load(sys.stdin)["download_url"])')
echo "== download matches upload: $(curl -fsS "$DL" | shasum -a 256 | awk '{print $1}' | grep -c "$SHA")"
echo "== delete account"; curl -sS -o /dev/null -w "%{http_code}\n" -X DELETE "$API/me" -H "$H" -H 'content-type: application/json' -d '{"confirm_email":"walk@napple.test"}'
echo "== blobs left: $(find "$DATA/blobs" -type f | wc -l | tr -d ' ')"
echo "== server log (codes printed by the dev mailer only)"; grep -v -e '^To:' -e '^ *[0-9]\{6\}$' -e '^-' -e '^Subject' -e '^$' -e 'Napple' -e 'code' -e 'password' -e 'email' "$DATA/server.log"
