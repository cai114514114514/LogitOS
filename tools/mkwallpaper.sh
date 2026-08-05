#!/usr/bin/env bash
# Regenerate fsroot/wallpaper.png from a macOS Desktop Picture. Apple's asset is
# gitignored (kept out of the repo); this re-derives it locally, like the fonts.
# Usage: tools/mkwallpaper.sh ["Wallpaper Name"]   (default: Sonoma)
set -e
NAME="${1:-Sonoma}"
SRC="/System/Library/Desktop Pictures/$NAME.heic"
[ -f "$SRC" ] || SRC="$(find '/System/Library/Desktop Pictures' -name "$NAME.heic" 2>/dev/null | head -1)"
[ -f "$SRC" ] || { echo "mkwallpaper: '$NAME' not found under /System/Library/Desktop Pictures"; exit 1; }
sips -s format png -z 800 1280 "$SRC" --out fsroot/wallpaper.png >/dev/null
echo "mkwallpaper: wrote fsroot/wallpaper.png (1280x800) from $SRC"
