#!/usr/bin/env python3
"""Drive two jamn_app --headless processes against each other on 127.0.0.1.

T4.3 proved the timing core across a real socket using a purpose-built peer
binary. This proves the same thing through the shipping application: its net
thread starts, exchanges notes and joins cleanly, with the audio device and
the runtime torn down in the right order around it. The peer binary cannot
show that, because it has neither an audio device nor a JUCE application
object to be ordered against.

jamn_app does the asserting - it exits nonzero if it never connected,
exchanged no notes, finished with a stuck note, or saw its runtime report a
fault - so this script's job is only to start the two, bound them in time,
and surface their output.

Registered as a ctest case under the `net` label (see
modules/jamn_app/CMakeLists.txt), not run by hand.

Usage: run_two_process_app.py <path-to-jamn_app> [net_ms]
"""

import socket
import subprocess
import sys
import time

# The "it hung" backstop, not the expected duration: jamn_app's own net
# session already stops on a wall clock, and ctest's TIMEOUT is the outer
# bound past this one. With no CI, a hung ctest is worse than a failed one.
TIMEOUT_SLACK_SECONDS = 40.0

# Long enough for the host to bind before the client's first connect
# attempt. jamn_app opens its listening socket before it touches an audio
# device precisely so this number does not have to cover a device open,
# which is milliseconds on a machine with no cards and is not on a machine
# with five. ENet retransmits the connect command regardless, so this only
# avoids leaning on that retry in the common case.
HOST_HEAD_START_SECONDS = 0.25

# jamn_app's own drain window after the play window closes, so trailing K=3
# burst copies and the last note-off have somewhere to land. Kept in step
# with kDrainUs in modules/jamn_app/src/main.cpp.
#
# jamn_app times its play window from the moment the link came up, not from
# its own start, so the head start above does not shorten either peer's run
# - it only delays when both of them begin. The budget below therefore
# carries it as added wall-clock time rather than assuming it overlaps.
DRAIN_SECONDS = 1.0


def pick_free_port() -> int:
    """Ask the OS for a free UDP port rather than hardcoding one.

    A fixed port is flaky the first time two runs overlap. Binding to port 0
    and releasing it leaves a small window before jamn_app rebinds, which is
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

    app_exe = sys.argv[1]
    net_ms = sys.argv[2] if len(sys.argv) > 2 else "1500"

    port = pick_free_port()
    budget = int(net_ms) / 1000.0 + DRAIN_SECONDS + HOST_HEAD_START_SECONDS + TIMEOUT_SLACK_SECONDS

    host = subprocess.Popen(
        [app_exe, "--headless", "--listen", str(port), "--net-ms", net_ms],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    time.sleep(HOST_HEAD_START_SECONDS)
    client = subprocess.Popen(
        [app_exe, "--headless", "--connect", f"127.0.0.1:{port}", "--net-ms", net_ms],
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
        # The cadence line is the other half of what this case is for: the
        # requirement is on the achieved interval, so a run that printed no
        # measurement proved nothing about it.
        elif "net thread cadence" not in (results[label].stdout or ""):
            failures.append(f"{label} reported no net thread cadence")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print(f"two-process jamn_app on 127.0.0.1:{port}: both peers clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
