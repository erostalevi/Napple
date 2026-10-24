#!/bin/bash
# Builds NappleStudio.app (ad-hoc signed). Usage: studio/build_app.sh [output-dir]
# The Swift scratch dir lives outside the repo tree because Finder/File Provider xattrs on
# ~/Documents break codesign ("detritus not allowed").
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
OUT="${1:-$HERE/dist}"
SCRATCH="${NAPPLE_STUDIO_SCRATCH:-${TMPDIR:-/tmp}/napple-studio-build}"
APP="$OUT/NappleStudio.app"

export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"
# Everything Studio's Build step needs: CLI, runtime, launcher (built here when missing; NAPPLE_REBUILD=1 forces).
if [ ! -x "$ROOT/target/release/napple" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then
  (cd "$ROOT" && cargo build --release -p napple-cli)
fi
if [ ! -x "$ROOT/runtime/build/napple-runtime" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then make -C "$ROOT/runtime"; fi
if [ ! -x "$ROOT/packaging/build/napple-launcher" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then make -C "$ROOT/packaging"; fi
for f in target/release/napple runtime/build/napple-runtime packaging/build/napple-launcher; do
  [ -x "$ROOT/$f" ] || { echo "missing $f" >&2; exit 1; }
done

cd "$HERE"
swift build -c release --scratch-path "$SCRATCH"
BIN="$(swift build -c release --scratch-path "$SCRATCH" --show-bin-path)/NappleStudio"

rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/NappleStudio"
# CLI + helpers side by side: `napple build` finds napple-runtime/napple-launcher next to itself.
cp "$ROOT/target/release/napple" "$ROOT/runtime/build/napple-runtime" "$ROOT/packaging/build/napple-launcher" "$APP/Contents/Resources/"
[ -f "$ROOT/compat/matrix.json" ] && cp "$ROOT/compat/matrix.json" "$APP/Contents/Resources/matrix.json"
cat > "$APP/Contents/Info.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Napple Studio</string>
<key>CFBundleDisplayName</key><string>Napple Studio</string>
<key>CFBundleIdentifier</key><string>app.napple.studio</string>
<key>CFBundleExecutable</key><string>NappleStudio</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
<key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
P
xattr -cr "$APP" 2>/dev/null || true
codesign --force --sign - --timestamp=none "$APP"
echo "Built $APP"
