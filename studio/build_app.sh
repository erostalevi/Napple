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
VERSION="${NAPPLE_VERSION:-0.1.0}"   # CFBundleShortVersionString; the release pipeline passes the build number as NAPPLE_BUILD
BUILD_NO="${NAPPLE_BUILD:-1}"

export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"
# Everything Studio's Build step needs: CLI, runtime, launcher (built here when missing; NAPPLE_REBUILD=1 forces).
CARGO_OUT="${CARGO_TARGET_DIR:-$ROOT/target}"   # honour CARGO_TARGET_DIR (CI and sandboxed builds keep it outside the tree)
if [ ! -x "$CARGO_OUT/release/napple" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then
  (cd "$ROOT" && cargo build --release -p napple-cli)
fi
if [ ! -x "$ROOT/runtime/build/napple-runtime" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then make -C "$ROOT/runtime"; fi
if [ ! -f "$ROOT/runtime/build-lib/libnapple-applet.dylib" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then make -C "$ROOT/runtime" applet-lib; fi
if [ ! -x "$ROOT/packaging/build/napple-launcher" ] || [ -n "${NAPPLE_REBUILD:-}" ]; then make -C "$ROOT/packaging"; fi
for f in "$CARGO_OUT/release/napple" "$ROOT/runtime/build/napple-runtime" "$ROOT/packaging/build/napple-launcher" "$ROOT/runtime/build-lib/libnapple-applet.dylib"; do
  [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }
done

cd "$HERE"
swift build -c release --scratch-path "$SCRATCH"
BIN="$(swift build -c release --scratch-path "$SCRATCH" --show-bin-path)/NappleStudio"

rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/NappleStudio"
# CLI + helpers side by side: `napple build` finds napple-runtime/napple-launcher next to itself.
cp "$CARGO_OUT/release/napple" "$ROOT/runtime/build/napple-runtime" "$ROOT/packaging/build/napple-launcher" "$ROOT/runtime/build-lib/libnapple-applet.dylib" "$APP/Contents/Resources/"   # the applet library beside the runtime: `napple build` bundles it into every game app
[ -x "$ROOT/runtime/build-ios/napple-runtime" ] && cp "$ROOT/runtime/build-ios/napple-runtime" "$APP/Contents/Resources/napple-runtime-ios"   # optional: tools/build_ios.sh
[ -x "$ROOT/runtime/build-ios-sim/napple-runtime" ] && cp "$ROOT/runtime/build-ios-sim/napple-runtime" "$APP/Contents/Resources/napple-runtime-ios-sim"   # optional: tools/build_ios.sh --sim
[ -f "$ROOT/compat/matrix.json" ] && cp "$ROOT/compat/matrix.json" "$APP/Contents/Resources/matrix.json"
# App icon (studio/packaging/icon.png -> AppIcon.icns)
if [ -f "$HERE/packaging/icon.png" ] && command -v iconutil >/dev/null 2>&1; then
  ICONSET="$SCRATCH/AppIcon.iconset"; rm -rf "$ICONSET"; mkdir -p "$ICONSET"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$HERE/packaging/icon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$HERE/packaging/icon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
fi
cat > "$APP/Contents/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Napple Studio</string>
<key>CFBundleDisplayName</key><string>Napple Studio</string>
<key>CFBundleIdentifier</key><string>app.napple.studio</string>
<key>CFBundleExecutable</key><string>NappleStudio</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>CFBundleVersion</key><string>$BUILD_NO</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>NSCameraUsageDescription</key><string>Games started from Studio can use your webcam as the Joy-Con IR camera, only when a game asks for it.</string>
<key>NSMicrophoneUsageDescription</key><string>Games started from Studio can listen to your microphone, only when a game asks for it.</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
<key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
<key>NSHighResolutionCapable</key><true/>
<key>CFBundleDocumentTypes</key><array><dict><key>CFBundleTypeName</key><string>Switch package</string><key>CFBundleTypeRole</key><string>Viewer</string><key>LSHandlerRank</key><string>Alternate</string><key>CFBundleTypeExtensions</key><array><string>nsp</string><string>xci</string><string>nsz</string><string>xcz</string></array></dict></array>
<key>NSSupportsAutomaticTermination</key><false/>
</dict></plist>
P
xattr -cr "$APP" 2>/dev/null || true
# Release bundle: drop local symbols (the debug builds in target/ and runtime/build keep them; no dSYM is shipped).
for f in "$APP/Contents/MacOS/NappleStudio" "$APP/Contents/Resources/napple" "$APP/Contents/Resources/napple-runtime" "$APP/Contents/Resources/napple-launcher"; do strip -x "$f"; done
codesign --force --sign - --timestamp=none --entitlements "$HERE/packaging/entitlements.plist" "$APP"
echo "Built $APP"
