#!/usr/bin/env bash
# Launch Phoenix Client from the repo root so shaders and runtime data resolve
# regardless of where you invoke this script from.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/linux-release/PhoenixClient"
if [ ! -x "$BIN" ] && [ -x "$ROOT/build/PhoenixClient" ]; then
    BIN="$ROOT/build/PhoenixClient"
fi

[ -x "$BIN" ] || { echo "Binary not found. Build first:  ./scripts/build.sh" >&2; exit 1; }

# The local data package may live in its installed location or in the old
# development/trash location. Keep the desktop launcher path-independent and
# select the first canonical binary tree (a directory containing world/).
if [ -z "${PHOENIX_CLIENT_DATA:-}" ]; then
    for candidate in \
        "$HOME/.local/share/Phoenix Client/data" \
        "$HOME/.local/share/Trash/files/Phoenix Client Linux/data" \
        "$ROOT/data"
    do
        if [ -d "$candidate/world" ] || [ -d "$candidate/World" ]; then
            PHOENIX_CLIENT_DATA="$candidate"
            export PHOENIX_CLIENT_DATA
            break
        fi
    done
fi

# An invalid explicit data path used to fall through to an empty runtime and
# leave the renderer showing only the sky. Fail early with a useful error
# instead: the engine requires at least the world/ directory to discover maps.
if [ -n "${PHOENIX_CLIENT_DATA:-}" ] && [ ! -d "$PHOENIX_CLIENT_DATA/world" ] && [ ! -d "$PHOENIX_CLIENT_DATA/World" ]; then
    echo "Invalid PHOENIX_CLIENT_DATA: $PHOENIX_CLIENT_DATA (expected a data directory containing world/)" >&2
    exit 1
fi
if [ -z "${PHOENIX_CLIENT_DATA:-}" ]; then
    echo "Phoenix Client data not found (expected a binary data/world tree)." >&2
    exit 1
fi

cd "$ROOT"
exec "$BIN" "$@"
