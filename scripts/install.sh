#!/usr/bin/env bash
# Copy the built module to a Move running Schwung.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
HOST="${MOVE_HOST:-move.local}"
DEST="/data/UserData/schwung/modules/tools/sculpt"
cd "$REPO_ROOT"
[ -d dist/sculpt ] || { echo "dist/sculpt missing - run ./scripts/build.sh first"; exit 1; }
echo "=== Installing Sculpt to $HOST:$DEST ==="
ssh "ableton@$HOST" "mkdir -p '$DEST'"
scp dist/sculpt/* "ableton@$HOST:$DEST/"
ssh "ableton@$HOST" "chmod -R a+rw '$DEST'"
echo "Done. On the Move: Shift+Vol+Jog (Schwung menu) > Tools > Sculpt."
echo "If it does not show up, rescan modules in Schwung Manager (http://$HOST:7700)."
