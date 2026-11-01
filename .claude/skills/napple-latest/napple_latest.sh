#!/bin/bash
# napple_latest.sh status | build [--incremental] [--no-tests] [--dest DIR]
#   status  list every work front (worktrees, branches not in main, stashes, uncommitted files) without changing anything;
#           local only: no fetch, pull, merge, stash, branch, commit or push, here or in build
#   build   clean-rebuild every component from the current working tree, run the test suites, package the result
#           into DIR (default ~/Desktop/NappleLATEST). An existing DIR is moved to the Trash, never deleted.
# The repository is the OUTER checkout (this script's ../../..). Napple/.git inside it is a stale nested repo:
# never run git from inside Napple/.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
NP="$ROOT/Napple"
G() { git -C "$ROOT" "$@"; }
export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:/opt/homebrew/bin:$PATH"

cmd="${1:-}"; shift || true
case "$cmd" in
status)
  echo "== repo: $ROOT  branch: $(G branch --show-current)  head: $(G log --oneline -1)"
  echo "== vs last-known origin/main (not fetched), ahead/behind: $(G rev-list --left-right --count main...origin/main 2>/dev/null | tr '\t' '/')"
  echo "== worktrees"; G worktree list
  echo "== branches with commits not in main"
  for b in $(G for-each-ref --format='%(refname:short)' refs/heads refs/remotes | grep -v -e '/HEAD$' -e '^origin$'); do
    n=$(G rev-list --count "main..$b" 2>/dev/null || echo 0)
    [ "$n" -gt 0 ] && echo "  $b: $n commit(s)  last: $(G log -1 --format='%h %cr %s' "$b")"
  done
  echo "== stashes"; G stash list
  echo "== uncommitted (main checkout)"; G status --short
  for wt in $(G worktree list --porcelain | awk '/^worktree /{print $2}' | grep -vx "$ROOT"); do
    echo "== uncommitted in worktree $wt"; git -C "$wt" status --short
  done
  echo "== sibling repos (not part of the Napple build)"
  for r in "$ROOT/LASRecomp" "$ROOT/MacNativeGames"; do
    [ -d "$r/.git" ] && echo "  $(basename "$r"): $(git -C "$r" log --oneline -1 2>/dev/null)  dirty files: $(git -C "$r" status --short 2>/dev/null | wc -l | tr -d ' ')"
  done
  exit 0 ;;
build) ;;
*) sed -n '2,7p' "$0"; exit 2 ;;
esac

CLEAN=1; TESTS=1; DEST="$HOME/Desktop/NappleLATEST"
while [ $# -gt 0 ]; do
  case "$1" in
    --incremental) CLEAN=0 ;; --no-tests) TESTS=0 ;; --dest) DEST="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac; shift
done

STAMP="$(date +%Y%m%d-%H%M%S)"
WORK="${TMPDIR:-/tmp}/napple-latest-$STAMP"   # outside ~/Documents and ~/Desktop: File Provider xattrs break codesign
STAGE="$WORK/NappleLATEST"; LOGS="$STAGE/logs"
mkdir -p "$LOGS"
RESULTS=()
step() {  # step NAME CMD... : run, log to logs/NAME.log, abort the build on failure
  local name="$1"; shift
  echo "-- $name"
  if ! "$@" >"$LOGS/$name.log" 2>&1; then
    echo "FAILED: $name (log: $LOGS/$name.log)"; tail -30 "$LOGS/$name.log"; exit 1
  fi
}
check() {  # check NAME CMD... : like step, but a failure is recorded and the build continues
  local name="$1"; shift
  echo "-- test $name"
  if perl -e 'alarm shift; exec @ARGV' 900 "$@" >"$LOGS/test-$name.log" 2>&1; then RESULTS+=("PASS $name")
  else RESULTS+=("FAIL $name (logs/test-$name.log)"); echo "   FAIL (see $LOGS/test-$name.log)"; fi
}

if [ "$CLEAN" = 1 ]; then
  for d in gfx audio input frontend runtime packaging; do step "clean-$d" make -C "$NP/$d" clean; done
  step clean-cargo cargo clean --release --manifest-path "$NP/Cargo.toml"
fi
# Order matters: runtime links gfx (lib + adapter object), audio, input, frontend.
step gfx       make -C "$NP/gfx" lib adapter
step audio     make -C "$NP/audio"
step input     make -C "$NP/input"
step frontend  make -C "$NP/frontend"
step runtime   make -C "$NP/runtime"
step launcher  make -C "$NP/packaging"
step cli       cargo build --release --workspace --manifest-path "$NP/Cargo.toml"
SCRATCH="$WORK/studio-scratch"
[ "$CLEAN" = 0 ] && SCRATCH="${TMPDIR:-/tmp}/napple-studio-build"
step studio env NAPPLE_STUDIO_SCRATCH="$SCRATCH" "$NP/studio/build_app.sh" "$STAGE"

if [ "$TESTS" = 1 ]; then
  check cargo    cargo test --workspace --manifest-path "$NP/Cargo.toml"
  for d in gfx audio input frontend runtime; do check "$d" make -C "$NP/$d" test; done
  check studio   bash -c "cd '$NP/studio' && swift test --scratch-path '$SCRATCH'"
fi

# Package: app, loose binaries, libraries + headers, data files.
mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/include" "$STAGE/compat"
cp "$NP/target/release/napple" "$NP/runtime/build/napple-runtime" "$NP/packaging/build/napple-launcher" "$STAGE/bin/"
cp "$NP/gfx/libnapple_gfx.a" "$NP/gfx/build/obj/adapter/gfx_napple.o" "$NP/audio/libnapple_audio.a" \
   "$NP/input/libnapple_input.a" "$NP/frontend/libnapple_frontend.a" "$STAGE/lib/"
for d in gfx audio input frontend; do cp -R "$NP/$d/include/." "$STAGE/include/"; done
cp "$NP/compat/matrix.json" "$STAGE/compat/"
cp -R "$NP/knownSignatures" "$STAGE/knownSignatures"
for f in README.md COMPATIBILITY.md THIRD_PARTY_NOTICES.md; do [ -f "$NP/$f" ] && cp "$NP/$f" "$STAGE/"; done

{
  echo "Napple LATEST build  $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo "source:  $ROOT  branch $(G branch --show-current)  commit $(G rev-parse --short HEAD)  ($(G log -1 --format=%s))"
  n=$(G status --porcelain | wc -l | tr -d ' ')
  echo "working tree: $n uncommitted path(s) INCLUDED in this build"; G status --short | sed 's/^/  /'
  echo "build: $([ "$CLEAN" = 1 ] && echo 'clean rebuild of every component' || echo incremental)"
  echo "toolchains: $(cargo --version) | $(swift --version 2>&1 | head -1) | $(xcrun clang --version | head -1)"
  echo "external libs (Homebrew, linked statically, not rebuilt): opus $(brew list --versions opus 2>/dev/null | awk '{print $2}'), libvpx $(brew list --versions libvpx 2>/dev/null | awk '{print $2}')"
  echo "tests:"; if [ "$TESTS" = 1 ]; then printf '  %s\n' "${RESULTS[@]}"; else echo "  skipped (--no-tests)"; fi
  echo "sha256:"; (cd "$STAGE" && shasum -a 256 bin/* lib/* NappleStudio.app/Contents/MacOS/NappleStudio | sed 's/^/  /')
} > "$STAGE/BUILD_INFO.txt"

# Install: move any previous copy to the Trash (recoverable), then copy, then re-check the signature
# (an iCloud-synced Desktop can add xattrs that invalidate it; re-sign ad hoc if so).
if [ -e "$DEST" ]; then mv "$DEST" "$HOME/.Trash/$(basename "$DEST")-replaced-$STAMP"; echo "previous build moved to ~/.Trash/$(basename "$DEST")-replaced-$STAMP"; fi
mkdir -p "$(dirname "$DEST")"
ditto "$STAGE" "$DEST"
if ! codesign --verify --deep --strict "$DEST/NappleStudio.app" 2>/dev/null; then
  xattr -cr "$DEST/NappleStudio.app"; codesign --force --sign - --timestamp=none "$DEST/NappleStudio.app" >/dev/null 2>&1
fi
codesign --verify --deep --strict "$DEST/NappleStudio.app" && echo "signature: ok" || echo "signature: INVALID at destination"
"$DEST/bin/napple" --version 2>/dev/null || "$DEST/bin/napple" --help 2>&1 | head -2
echo; cat "$DEST/BUILD_INFO.txt" | sed -n '1,4p'; echo "tests:"; printf '  %s\n' "${RESULTS[@]:-skipped}"
echo "DONE -> $DEST   (staging + full logs: $STAGE)"
