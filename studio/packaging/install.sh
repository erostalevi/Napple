#!/bin/bash
# Napple installer: copies Napple Studio to /Applications (or ~/Applications when that is not writable), clears the download quarantine
# flag and opens it. Studio sets up the engine and the NAPPLE_HOME / NAPPLE_CLI variables itself on first launch.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/NappleStudio.app"
[ -d "$SRC" ] || { echo "NappleStudio.app not found next to install.sh" >&2; exit 1; }
DEST_DIR="/Applications"
[ -w "$DEST_DIR" ] || { DEST_DIR="$HOME/Applications"; mkdir -p "$DEST_DIR"; }
DEST="$DEST_DIR/NappleStudio.app"
pkill -x NappleStudio 2>/dev/null || true
rm -rf "$DEST"
ditto "$SRC" "$DEST"
xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true
echo "Installed to $DEST"
open "$DEST"
