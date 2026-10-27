#!/bin/bash
# tools/e2e/run.sh <las|smo|acnh|mk8d|all> [options]  - end-to-end run of the whole product per title (docs/guides/testing.md#end-to-end).
# Builds the title's .app through Napple Studio's own build path (studio napple-studio-build: KeysetManager + NappleCLI.build, no hand-built data root),
# launches it through the launcher, and prints a per-step PASS/FAIL table plus a JSON summary. See tools/e2e/e2e.py for the steps and options.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export NAPPLE_ROOT="$ROOT"
# Pillow (golden diffs) lives in the system python's user site (no -I: it would hide it).
PY=/usr/bin/python3; [ -x "$PY" ] || PY=python3
exec "$PY" "$HERE/e2e.py" "$@"
