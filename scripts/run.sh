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

cd "$ROOT"
exec "$BIN" "$@"
