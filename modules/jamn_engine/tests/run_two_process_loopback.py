#!/usr/bin/env python3
"""Drive two jamn_loopback_peer processes against each other on 127.0.0.1.

This is T4.3's checkpoint: the timing core running across a real socket
between two processes that share no memory. The peers do the asserting -
each exits nonzero if it never connected, exchanged no notes, finished with
a stuck note, or saw its runtime report a fault - so this script's job is
only to start them, bound them in time, and surface their output.

Registered as a ctest case under the `net` label (see
modules/jamn_engine/CMakeLists.txt), not run by hand.

Usage: run_two_process_loopback.py <path-to-jamn_loopback_peer> [play_ms] [drain_ms]
"""

import socket
import subprocess
import sys
import time

# Generous relative to the peers' own play+drain budget: this is the "it
# hung" backstop, not the expected duration. With no CI, a hung ctest is
# worse than a failed one, so every layer here is bounded - this timeout,
# the peers' own wall-clock stop, and ctest's TIMEOUT property.
TIMEOUT_SLACK_SECONDS = 30.0

# Long enough for the host to bind before the client's first connect
# attempt. ENet retransmits the connect command anyway, so this only avoids
# leaning on that retry for the common case.
HOST_HEAD_START_SECONDS = 0.25


def pick_free_port() -> int:
    """Ask the OS for a free UDP port rather than hardcoding one.

    A fixed port is flaky the first time two runs overlap. Binding to port 0
    and releasing it leaves a small window before the peer rebinds, which is
    still strictly better than picking a number and hoping.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def report(label: str, completed: subprocess.CompletedProcess) -> None:
    print(f"--- {label} (exit {completed.returncode}) ---")
    if completed.stdout:
        print(completed.stdout.rstrip())
    if completed.stderr:
        print(completed.stderr.rstrip(), file=sys.stderr)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2

    peer_exe = sys.argv[1]
    play_ms = sys.argv[2] if len(sys.argv) > 2 else "2000"
    drain_ms = sys.argv[3] if len(sys.argv) > 3 else "1000"

    port = pick_free_port()
    budget = (int(play_ms) + int(drain_ms)) / 1000.0 + TIMEOUT_SLACK_SECONDS

    common = ["--port", str(port), "--play-ms", play_ms, "--drain-ms", drain_ms]
    host = subprocess.Popen(
        [peer_exe, "--role", "host", *common],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    time.sleep(HOST_HEAD_START_SECONDS)
    client = subprocess.Popen(
        [peer_exe, "--role", "client", *common],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )

    failures = []
    results = {}
    for label, process in (("host", host), ("client", client)):
        try:
            stdout, stderr = process.communicate(timeout=budget)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            failures.append(f"{label} did not exit within {budget:.1f}s")
        results[label] = subprocess.CompletedProcess(
            process.args, process.returncode, stdout, stderr
        )

    for label in ("host", "client"):
        report(label, results[label])
        if results[label].returncode != 0:
            failures.append(f"{label} exited {results[label].returncode}")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print(f"two-process loopback on 127.0.0.1:{port}: both peers clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
