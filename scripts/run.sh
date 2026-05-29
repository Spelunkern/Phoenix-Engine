#!/usr/bin/env bash
# Launch Phoenix Engine from the repo root so shaders/compiled/ and Data/
# resolve regardless of where you invoke this script from.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/PhoenixEngine"

[ -x "$BIN" ] || { echo "Binary not found. Build first:  ./scripts/build.sh" >&2; exit 1; }

cd "$ROOT"
exec "$BIN" "$@"
