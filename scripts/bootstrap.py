#!/usr/bin/env python3
"""Warm deps-cache/, the shared source checkout cache for Catch2, ENet and
JUCE, so every preset's configure step - not just one - makes no network
request afterwards. Safe to re-run: only dependencies actually missing from
deps-cache/ get fetched.

Only *sources* are shared, via FETCHCONTENT_SOURCE_DIR_<NAME> (set the same
way in CMakePresets.json). Each preset still builds its own compiled
objects in its own build-*/ directory - sharing those too would mean the
ASan/TSan presets reusing objects built with completely different compiler
flags, which is wrong, not just wasteful.

This does not detect a stale checkout if a dependency's pin in
CMakeLists.txt changes later - it only checks whether deps-cache/<name>-src
exists. Delete the relevant deps-cache/<name>-src by hand after bumping a
pin, and this will re-fetch it.

Run this once after cloning, or before working somewhere without network
access.
"""
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEPS_CACHE = REPO_ROOT / "deps-cache"
SCRATCH = REPO_ROOT / ".bootstrap-scratch"

# CMakeLists.txt FetchContent_Declare() name -> its FETCHCONTENT_SOURCE_DIR_<NAME> suffix
DEPS = {"catch2": "CATCH2", "enet": "ENET", "juce": "JUCE"}


def main():
    missing = {n for n in DEPS if not (DEPS_CACHE / f"{n}-src").is_dir()}
    if not missing:
        print("deps-cache/ already has every dependency - nothing to fetch.")
        return 0

    # Only reach the ENet/JUCE FetchContent_Declare calls (behind
    # JAMN_CORE_ONLY) if one of them is actually what's missing - avoids
    # the JUCE module's own juceaide build (real compile time) when only
    # Catch2 needs fetching.
    core_only = "OFF" if missing & {"enet", "juce"} else "ON"

    print(f"== bootstrap: fetching {', '.join(sorted(missing))} into deps-cache/ ==")
    args = [
        "cmake", "-S", str(REPO_ROOT), "-B", str(SCRATCH), "-G", "Ninja",
        f"-DJAMN_CORE_ONLY={core_only}",
        f"-DFETCHCONTENT_BASE_DIR={SCRATCH}",
    ]
    # Anything already cached is reused in place rather than re-fetched into
    # the scratch dir, even though this configure only runs at all because
    # something else is missing.
    for name, suffix in DEPS.items():
        if name not in missing:
            args.append(f"-DFETCHCONTENT_SOURCE_DIR_{suffix}={DEPS_CACHE / (name + '-src')}")

    result = subprocess.run(args, cwd=REPO_ROOT)
    if result.returncode != 0:
        print("bootstrap failed - see cmake output above.", file=sys.stderr)
        return result.returncode

    DEPS_CACHE.mkdir(exist_ok=True)
    for name in missing:
        src = SCRATCH / f"{name}-src"
        if not src.is_dir():
            print(f"bootstrap: expected {src} after configure, not found.", file=sys.stderr)
            return 1
        shutil.move(str(src), str(DEPS_CACHE / f"{name}-src"))

    shutil.rmtree(SCRATCH, ignore_errors=True)
    print(f"deps-cache/ is warm ({', '.join(sorted(missing))} fetched). Any "
          "preset's configure from here makes no network request.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
