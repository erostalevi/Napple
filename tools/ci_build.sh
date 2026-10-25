#!/bin/bash
# Builds every in-repo dependency in the right order, then the CLI, Studio and the release artifacts. macOS (Apple Silicon) only.
#   tools/ci_build.sh [output-dir]      default output: <repo>/dist
# Env: NAPPLE_VERSION (e.g. 0.1.42), NAPPLE_BUILD (build number), NAPPLE_SIGN_IDENTITY (optional Developer ID, see studio/packaging/make_dmg.sh),
#      NAPPLE_SKIP_TESTS=1 to skip the (non-blocking) test run.
# Needs: Xcode command line tools, rustup/cargo, `brew install opus libvpx` (both are linked statically into the runtime).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/dist}"
export NAPPLE_VERSION="${NAPPLE_VERSION:-0.1.0}" NAPPLE_BUILD="${NAPPLE_BUILD:-1}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
step() { printf '\n==> %s\n' "$*"; }

cd "$ROOT"
step "CLI (napple)"
cargo build --release -p napple-cli --locked

step "libraries: input, audio, frontend, gfx"
make -C input -j"$JOBS"
make -C audio -j"$JOBS"
make -C frontend -j"$JOBS"
make -C gfx -j"$JOBS" lib adapter

step "runtime + launcher"
make -C runtime -j"$JOBS"
make -C packaging

if [ -z "${NAPPLE_SKIP_TESTS:-}" ]; then
  step "tests (non-blocking)"
  { cargo test --release --locked && make -C runtime test-host; } || echo "::warning::tests failed, the build is still published"
fi

step "Napple Studio.app"
rm -rf "$OUT"; mkdir -p "$OUT"
"$ROOT/studio/build_app.sh" "$OUT"

step "release archives"
"$ROOT/studio/packaging/make_release.sh" "$OUT"
step "done"; ls -lh "$OUT"
