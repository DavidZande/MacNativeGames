#!/bin/bash
# Local change: rebuild only the game binary in build_app.py's work tree (seconds to a minute), for testing with
# scripts/bench.py. The packaged app is not touched: replacing or re-signing its 16 GB bundle makes macOS scan all of it
# again ("Verifying..."), so rebuild the app with build_app.py only to hand a version to the player.
set -euo pipefail
GAME="$(cd "$(dirname "$0")/.." && pwd)"
TREE="${TOTK_TREE:-$HOME/Documents/TearsOfKingdom/TOTK_build/totk}"
[ -d "$TREE/rewritten" ] || { echo "no work tree at $TREE (run build_app.py once)"; exit 1; }
rsync -a --exclude=/extracted --exclude=/rewritten --exclude=/build --exclude=/analysis --exclude=/.venv --exclude=/tooling --exclude=/user-data "$GAME/" "$TREE/"
mkdir -p "$TREE/build/shader-service"
[ -x "$TREE/build/shader-service/totk_xlate" ] || cp -c "$TREE/prebuilt/macos-arm64/totk_xlate" "$TREE/build/shader-service/totk_xlate"
cd "$TREE" && PATH="/opt/homebrew/bin:/usr/local/bin:$PATH" make runtime 2>&1 | tail -5
ls -la "$TREE/build/totk-native"
