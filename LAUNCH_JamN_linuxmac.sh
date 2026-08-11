#!/usr/bin/env bash
# Runs the already-built jamn_app binary from the linux-ninja preset (build/).
# Does not build or configure anything - if the binary is missing or stale,
# run ./build_and_test_linuxmac.sh first. Any arguments are passed through,
# e.g. `./LAUNCH_JamN_linuxmac.sh --headless`.
set -euo pipefail
cd "$(dirname "$0")"

BIN="build/modules/jamn_app/jamn_app"

if [[ ! -x "$BIN" ]]; then
    echo "$BIN not found or not executable." >&2
    echo "Build it first: ./build_and_test_linuxmac.sh" >&2
    exit 1
fi

exec "$BIN" "$@"
