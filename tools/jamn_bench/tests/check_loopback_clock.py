#!/usr/bin/env python3
"""Smoke-check jamn_bench's loopback-clock backend.

Runs the backend briefly and asserts it actually took a reading. This is a
liveness check that the backend still works, not acceptance criterion #1 -
that reading is 30 wall-clock minutes and stays a manual step:

    jamn_bench --backend loopback-clock --duration-seconds 1800 > bench.json

The failure this exists to catch is a silent one: if the peers never
connect or the clock never locks, the backend reports
clock_offset_p99_us = -1 ("not measured") and exits 0. That is not a
passing run, and asserting on a real number is what distinguishes them.

Usage: check_loopback_clock.py <path-to-jamn_bench> [duration_seconds]
"""

import json
import subprocess
import sys

# Deliberately looser than criterion #1's 500us. This runs for seconds on
# whatever else the machine is doing, so holding it to the criterion would
# make it flaky without making it stricter in any meaningful sense - the
# criterion is a 30-minute reading on a quiet box, taken by hand. What this
# bound catches is the backend being broken by a wide margin, and the "no
# reading at all" check below is the one that catches it being broken
# silently.
MAX_PLAUSIBLE_P99_US = 2000.0


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    bench_exe = sys.argv[1]
    duration = sys.argv[2] if len(sys.argv) > 2 else "6"

    completed = subprocess.run(
        [bench_exe, "--backend", "loopback-clock", "--duration-seconds", duration],
        capture_output=True, text=True, timeout=float(duration) + 60.0,
    )
    if completed.returncode != 0:
        print(completed.stdout)
        print(completed.stderr, file=sys.stderr)
        print(f"FAIL: jamn_bench exited {completed.returncode}", file=sys.stderr)
        return 1

    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        print(completed.stdout)
        print(f"FAIL: output was not valid JSON: {error}", file=sys.stderr)
        return 1

    rows = [r for r in payload.get("results", []) if r.get("backend") == "loopback-clock"]
    if len(rows) != 1:
        print(f"FAIL: expected exactly one loopback-clock row, got {len(rows)}", file=sys.stderr)
        return 1

    row = rows[0]
    p99 = row.get("clock_offset_p99_us", -1.0)
    print(f"loopback-clock p99: {p99} us")
    print(f"notes: {row.get('notes', '')}")

    if p99 < 0.0:
        print("FAIL: no reading taken - the peers never connected or the clock never locked",
              file=sys.stderr)
        return 1
    if p99 > MAX_PLAUSIBLE_P99_US:
        print(f"FAIL: p99 {p99}us exceeds {MAX_PLAUSIBLE_P99_US}us on a loopback socket",
              file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
