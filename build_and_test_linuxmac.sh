#!/usr/bin/env bash
# The gate: run this before considering any change done. No OS-specific
# CMake exists in this repo (JUCE/ENet handle their own per-platform
# backends internally), so this script is untested-but-expected-to-work on
# macOS unchanged - it uses nothing Linux-specific either.
#
# core-only runs first because it's the whole point of the module split:
# jamn_core/jamn_proto/jamn_net/jamn_dsp/jamn_engine/jamn_session must
# build and test without JUCE. Prove that cheaply before paying for the
# full JUCE-linking build.
set -euo pipefail
cd "$(dirname "$0")"

echo "== module boundary check =="
python3 tools/check_module_boundaries.py

echo "== core-only: configure =="
cmake --preset core-only
echo "== core-only: build =="
ninja -C build-core-only
echo "== core-only: test =="
ctest --preset core-only -L fast

echo "== linux-ninja: configure =="
cmake --preset linux-ninja
echo "== linux-ninja: build =="
ninja -C build
echo "== linux-ninja: test (must not link JUCE) =="
ctest --preset linux-ninja -L fast

echo "== linux-ninja: test app (JUCE-linked, e.g. jamn_app_smoke) =="
ctest --preset linux-ninja -L app

echo
echo "All checks passed."
