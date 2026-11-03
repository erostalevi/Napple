#!/bin/bash
# Publish local main to origin/main as ONE squashed commit on the `publish` branch, without the private process files.
# Usage: publish.sh "<subject>" [body-file]   (run from anywhere; never pushes if the public tree fails its checks)
#        publish.sh --dry-run [dir]           (build and check the public tree in a scratch worktree, print where it is, push nothing)
set -euo pipefail
ROOT=/Users/neto/napple; cd "$ROOT"
DRY=0
if [ "${1:-}" = "--dry-run" ]; then DRY=1; SUBJ=dry-run; WT="${2:-$(mktemp -d "${TMPDIR:-/tmp}/napple-public.XXXXXX")}"; else SUBJ="${1:?subject}"; WT=""; fi
BODY="${2:-}"; [ $DRY = 1 ] && BODY=""
if [ $DRY = 0 ]; then
  git fetch -q origin
  [ "$(git rev-parse origin/main)" = "$(git rev-parse publish)" ] || { echo "origin/main moved away from publish: stop"; exit 2; }
  WT=$(mktemp -d "${TMPDIR:-/tmp}/napple-publish.XXXXXX")
fi
rmdir "$WT" 2>/dev/null || true
git worktree add -q --detach "$WT" main
[ $DRY = 0 ] && trap 'git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1 || true' EXIT
cd "$WT"
LOG="$WT/../$(basename "$WT").log"

# 1. private workflow: never public
git rm -r -q --ignore-unmatch -- .claude integrators orchestrations docs/process CLAUDE.md AGENTS.md
# links into the private archive
sed -i '' '/process\/archive\/README\.md/d' docs/index.md docs/archive/changes/README.md 2>/dev/null || true

# 2. change notes are history written by the workflow: drop its vocabulary mechanically (names and wording only)
for f in docs/archive/changes/*-claude-*.md; do [ -e "$f" ] || continue; git mv -f "$f" "${f//-claude-/-}"; done
git ls-files -z | xargs -0 grep -lIZ -E '[0-9]{4}-[0-9]{2}-[0-9]{2}-claude-' 2>/dev/null | xargs -0 perl -pi -e 's/(\d{4}-\d{2}-\d{2})-claude-/$1-/g' 2>/dev/null || true
git ls-files -z 'docs/archive/changes/*' | xargs -0 perl -pi -e '
s{(?<![.\w])claude/}{}g;
s{`?(?:docs/process/archive/)?integrators/(?:done/)?claude-[\w.-]+?(?:\.md)?`?(?=[\s.,;:)]|$)}{the hand-off notes}g;
s{`?(?:docs/process/archive/)?orchestrations/(?:done/)?([\w-]+)\.md`?}{assignment `$1`}g;
s{`?\.claude/(?:skills/)?([\w-]+)[\w./-]*`?}{the private `$1` tooling}g;
s{\borchestrate/}{batch/}g;
s{\b([Tt]he |[Aa]n? )?[Ii]ntegrator files?\b}{ my $a=$1//""; ($a=~/^[Aa]n?/ ? "the ":$a)."hand-off notes" }ge;
s{\bIntegrator\b}{Hand-off}g; s{\bintegrators/}{hand-off notes/}g; s{\bintegrators\b}{hand-off notes}g; s{\bintegrator\b}{integration}g;
s{\borchestrations/}{assignments/}g; s{\bOrchestrations?\b}{Assignment}g; s{\borchestrations\b}{assignments}g; s{\borchestration\b}{assignment}g;
s{\borchestrator\x27s\b}{maintainer\x27s}g; s{\borchestrator\b}{maintainer}g; s{\borchestrated\b}{assigned}g; s{\borchestrate\b}{assign}g;
s{\bClaude sessions?\b}{sessions}g; s{\bClaude\b}{the assistant}g;'

# 3. a contributor can never commit the private folders
cat >> .gitignore <<'EOF'

# Local agent and workflow folders: never part of this repository
.claude/
integrators/
orchestrations/
docs/process/
EOF
git add -A

# 4. guard: no Claude or workflow vocabulary anywhere else in the public tree (file names and contents)
ALLOW='^(\.gitignore|tools/support/gen_support\.py)$'   # .gitignore lists the folders; gen_support.py's --refresh-snapshot default path
HITS=$( { git ls-files | grep -i -E 'claude|orchestrat|integrator' || true; git ls-files -z | xargs -0 grep -lIi -E 'claude|orchestrat|integrator' 2>/dev/null || true; } | { grep -v -E "$ALLOW" || true; } | sort -u)
[ -z "$HITS" ] || { echo "public tree still names Claude / the private workflow in:"; echo "$HITS" | head -40; exit 3; }

# 5. the usual checks
python3 -I tools/check_docs.py >"$LOG" 2>&1 || { echo "check_docs FAILED on the public tree:"; grep FAIL "$LOG" | head -20; exit 3; }
python3 -I tools/support/gen_support.py --check >/dev/null 2>&1 || { echo "gen_support --check FAILED"; exit 3; }
if python3 -c "import yaml" 2>/dev/null; then python3 tools/check_contributor_kit.py --root . >/dev/null 2>&1 || { echo "check_contributor_kit FAILED"; python3 tools/check_contributor_kit.py --root . | tail -5; exit 3; }; else echo "note: PyYAML missing, contributor-kit check skipped"; fi
TREE=$(git write-tree)
if [ $DRY = 1 ]; then echo "public tree OK: $WT (tree $TREE); remove with: git worktree remove --force $WT"; exit 0; fi
cd "$ROOT"
MSG=$(printf '%s\n\n%s\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n' "$SUBJ" "$( [ -n "$BODY" ] && cat "$BODY" )")
C=$(printf '%s' "$MSG" | git commit-tree "$TREE" -p publish)
git branch -f publish "$C"
git push origin publish:main
