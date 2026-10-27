#!/bin/bash
# Turns <dir>/NappleStudio.app into the release assets (fixed names, so .../releases/latest/download/<name> never changes):
#   Napple-Studio.dmg          drag-and-drop installer (Studio then sets up the engine and NAPPLE_* variables on first launch)
#   Napple-macOS-arm64.zip     NappleStudio.app + bin/ (napple, napple-runtime, napple-launcher, libnapple-applet.dylib) + install.sh + notices
#   SHA256SUMS.txt
# Usage: studio/packaging/make_release.sh <dir containing NappleStudio.app>
# Optional signing: NAPPLE_SIGN_IDENTITY="Developer ID Application: ..." re-signs the app with the hardened runtime, and
#   NAPPLE_NOTARY_PROFILE=<notarytool keychain profile> additionally notarizes + staples it. Without them the app is ad-hoc signed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$(cd "${1:?usage: make_release.sh <dir containing NappleStudio.app>}" && pwd)"
APP="$OUT/NappleStudio.app"
[ -d "$APP" ] || { echo "missing $APP" >&2; exit 1; }
VERSION="${NAPPLE_VERSION:-0.1.0}"

if [ -n "${NAPPLE_SIGN_IDENTITY:-}" ]; then
  echo "signing with: $NAPPLE_SIGN_IDENTITY"
  for f in "$APP"/Contents/Resources/napple "$APP"/Contents/Resources/napple-runtime "$APP"/Contents/Resources/napple-launcher; do
    codesign --force --options runtime --timestamp --entitlements "$HERE/entitlements.plist" --sign "$NAPPLE_SIGN_IDENTITY" "$f"
  done
  codesign --force --options runtime --timestamp --entitlements "$HERE/entitlements.plist" --sign "$NAPPLE_SIGN_IDENTITY" "$APP"
  codesign --verify --strict --verbose=2 "$APP"
fi
if [ -n "${NAPPLE_NOTARY_PROFILE:-}" ] && [ -n "${NAPPLE_SIGN_IDENTITY:-}" ]; then
  ditto -c -k --keepParent "$APP" "$OUT/notarize.zip"
  xcrun notarytool submit "$OUT/notarize.zip" --keychain-profile "$NAPPLE_NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  rm -f "$OUT/notarize.zip"
fi

# ---- DMG ------------------------------------------------------------------------------------------------------------
VENV="${TMPDIR:-/tmp}/napple-dmgbuild-venv"
[ -x "$VENV/bin/dmgbuild" ] || { python3 -m venv "$VENV"; "$VENV/bin/pip" install --quiet dmgbuild; }
BG="${TMPDIR:-/tmp}/napple-dmg-bg.tiff"
tiffutil -cathidpicheck "$HERE/dmg-background.png" "$HERE/dmg-background@2x.png" -out "$BG"   # retina-aware background
ICNS="$APP/Contents/Resources/AppIcon.icns"
rm -f "$OUT/Napple-Studio.dmg"
"$VENV/bin/dmgbuild" -s "$HERE/dmg_settings.py" -D app="$APP" -D bg="$BG" -D icon="$([ -f "$ICNS" ] && echo "$ICNS")" "Napple Studio" "$OUT/Napple-Studio.dmg"
if [ -n "${NAPPLE_SIGN_IDENTITY:-}" ]; then
  codesign --force --timestamp --sign "$NAPPLE_SIGN_IDENTITY" "$OUT/Napple-Studio.dmg"
  [ -n "${NAPPLE_NOTARY_PROFILE:-}" ] && { xcrun notarytool submit "$OUT/Napple-Studio.dmg" --keychain-profile "$NAPPLE_NOTARY_PROFILE" --wait; xcrun stapler staple "$OUT/Napple-Studio.dmg"; }
fi

# ---- ZIP ------------------------------------------------------------------------------------------------------------
STAGE="$OUT/Napple-$VERSION"
rm -rf "$STAGE"; mkdir -p "$STAGE/bin"
ditto "$APP" "$STAGE/NappleStudio.app"
cp "$APP/Contents/Resources/napple" "$APP/Contents/Resources/napple-runtime" "$APP/Contents/Resources/napple-launcher" "$APP/Contents/Resources/libnapple-applet.dylib" "$STAGE/bin/"   # the applet library beside the runtime: napple build bundles it into every game app
[ -f "$APP/Contents/Resources/matrix.json" ] && cp "$APP/Contents/Resources/matrix.json" "$STAGE/bin/"
cp "$HERE/install.sh" "$STAGE/install.sh"; chmod +x "$STAGE/install.sh"
cp "$ROOT/README.md" "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
[ -f "$ROOT/gfx/THIRD_PARTY_NOTICES.md" ] && cp "$ROOT/gfx/THIRD_PARTY_NOTICES.md" "$STAGE/THIRD_PARTY_NOTICES.gfx.md"
printf 'Napple %s\n\nInstall:  ./install.sh      (copies Napple Studio to /Applications and opens it; Studio does the rest on first launch)\nOr drag NappleStudio.app anywhere you like and open it.\nbin/ holds the standalone napple CLI, runtime and launcher if you want them without Studio.\n' "$VERSION" > "$STAGE/START-HERE.txt"
rm -f "$OUT/Napple-macOS-arm64.zip"
(cd "$OUT" && ditto -c -k --keepParent "Napple-$VERSION" "Napple-macOS-arm64.zip")
rm -rf "$STAGE"

(cd "$OUT" && shasum -a 256 Napple-Studio.dmg Napple-macOS-arm64.zip > SHA256SUMS.txt)
echo "assets in $OUT:"; ls -lh "$OUT"/Napple-Studio.dmg "$OUT"/Napple-macOS-arm64.zip "$OUT"/SHA256SUMS.txt
