#!/bin/bash
# Builds a runtime data directory from a game container (stopgap pipeline: Python rewriter + table writer).
#   tools/prepare_game.sh <game.nsp|xci> <prod.keys> [--romfs]
# Output: work/games/<titleid>/<buildid16>/{manifest.json, main.npdm, modules/, [romfs.bin]}; prints the directory.
# --romfs also streams romfs.bin (several GB: keep one game's romfs.bin on disk at a time).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="$1"; KEYS="$2"; WANT_ROMFS="${3:-}"
export PATH=/opt/homebrew/opt/rustup/bin:$PATH
NAPPLE="$ROOT/target/release/napple"; PY="$ROOT/work/venv/bin/python"
TMP="$ROOT/work/prep.$$"; mkdir -p "$TMP/exefs" "$TMP/rewritten"
trap 'rm -rf "$TMP"' EXIT
"$NAPPLE" build --dump-exefs "$GAME" -k "$KEYS" -o "$TMP/exefs" >"$TMP/dump.log"
TITLE=$(grep '^title id:' "$TMP/dump.log" | awk '{print $3}')
BUILD=$("$PY" -c "print(open('$TMP/exefs/main','rb').read()[0x40:0x50].hex()[:16])")
OUT="$ROOT/work/games/$TITLE/$BUILD"; mkdir -p "$OUT"
if [ "${REWRITER:-rust}" = "python" ]; then   # stopgap reference rewriter
  for m in main sdk subsdk0 subsdk1 subsdk2 subsdk3; do
    [ -f "$TMP/exefs/$m" ] || continue
    DM_RULESET=native DM_PRIVATE_GATES=0 "$PY" "$ROOT/tools/refpy/patch_nso.py" "$TMP/exefs/$m" "$TMP/rewritten" "$m" >/dev/null
  done
else
  "$NAPPLE" rewrite "$GAME" -k "$KEYS" -o "$TMP/rewritten" >"$TMP/rewrite.log"
  rm -f "$TMP"/rewritten/rtld.*
fi
"$PY" "$ROOT/tools/prepare_tables.py" "$TMP/rewritten" --exefs "$TMP/exefs" --out "$OUT/modules" --title-id "$TITLE" --npdm "$TMP/exefs/main.npdm" >/dev/null
[ "$WANT_ROMFS" = "--romfs" ] && "$NAPPLE" build --dump-romfs "$GAME" -k "$KEYS" -o "$OUT/romfs.bin"
# Per-game runtime data (game.json: window title and the NVN API version the game checks; defaults 55.15). Existing files are kept: edit them by hand
# (Super Mario Odyssey needs 53/13, see docs/runtime-services.md).
[ -f "$OUT/game.json" ] || printf '{"title_name": "%s", "nvn_major": 55, "nvn_minor": 15}\n' "$TITLE" > "$OUT/game.json"
# Shared fonts for pl:u, once per machine, from the user's firmware: FIRMWARE=<Firmware.zip|dir> tools/prepare_game.sh ...
FONTS="$HOME/Library/Application Support/Napple/system/fonts"
if [ -n "${FIRMWARE:-}" ] && [ ! -d "$FONTS" ]; then "$NAPPLE" build --dump-fonts "$FIRMWARE" -k "$KEYS" -o "$FONTS" >/dev/null; fi
echo "$OUT"
