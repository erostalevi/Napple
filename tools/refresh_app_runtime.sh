#!/bin/sh
# tools/refresh_app_runtime.sh <App.app>...  - replaces the runtime (and launcher) inside already built Napple apps with the current
# runtime/build and packaging/build binaries and re-signs them ad hoc, so runtime changes can be tried through the real double-click path
# without a full `napple build` (which re-derives the RomFS and rewrites the modules). Data under Contents/Resources/game is untouched.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
for app in "$@"; do
  [ -x "$app/Contents/MacOS/napple-runtime" ] || { echo "not a Napple app: $app" >&2; exit 2; }
  cp "$root/runtime/build/napple-runtime" "$app/Contents/MacOS/napple-runtime"
  [ -x "$root/packaging/build/napple-launcher" ] && cp "$root/packaging/build/napple-launcher" "$app/Contents/MacOS/napple-launcher"
  xattr -cr "$app" 2>/dev/null || true
  codesign --force --deep -s - "$app" >/dev/null 2>&1 || { echo "codesign failed for $app" >&2; exit 1; }
  echo "refreshed: $app"
done
