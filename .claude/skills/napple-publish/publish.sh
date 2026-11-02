#!/bin/bash
# Publish local main to origin/main as ONE squashed commit on the `publish` branch, without the private process files.
# Usage: publish.sh "<subject>" [body-file]   (run from anywhere; never pushes if the public tree fails its checks)
set -euo pipefail
ROOT=/Users/neto/napple; cd "$ROOT"
SUBJ="${1:?subject}"; BODY="${2:-}"
git fetch -q origin
[ "$(git rev-parse origin/main)" = "$(git rev-parse publish)" ] || { echo "origin/main moved away from publish: stop"; exit 2; }
WT=$(mktemp -d "${TMPDIR:-/tmp}/napple-publish.XXXXXX")
git worktree add -q --detach "$WT" main
trap 'git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || true' EXIT
cd "$WT"
git rm -r -q --ignore-unmatch -- .claude integrators orchestrations docs/process/archive
# links into the private archive
sed -i '' '/process\/archive\/README\.md/d' docs/index.md docs/archive/changes/README.md 2>/dev/null || true
git add -A
python3 -I tools/check_docs.py >/tmp/napple-publish-docs.log 2>&1 || { echo "check_docs FAILED on the public tree:"; grep FAIL /tmp/napple-publish-docs.log | head -20; exit 3; }
python3 -I tools/support/gen_support.py --check >/dev/null 2>&1 || { echo "gen_support --check FAILED"; exit 3; }
if python3 -c "import yaml" 2>/dev/null; then python3 tools/check_contributor_kit.py --root . >/dev/null 2>&1 || { echo "check_contributor_kit FAILED"; python3 tools/check_contributor_kit.py --root . | tail -5; exit 3; }; else echo "note: PyYAML missing, contributor-kit check skipped"; fi
TREE=$(git write-tree)
cd "$ROOT"
MSG=$(printf '%s\n\n%s\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n' "$SUBJ" "$( [ -n "$BODY" ] && cat "$BODY" )")
C=$(printf '%s' "$MSG" | git commit-tree "$TREE" -p publish)
git branch -f publish "$C"
git push origin publish:main
