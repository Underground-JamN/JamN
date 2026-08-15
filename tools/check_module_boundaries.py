#!/usr/bin/env python3
"""jamn_core, jamn_proto, jamn_net, jamn_dsp, jamn_engine and jamn_session
must never include JUCE (see AGENTS.md). Python, not bash/PowerShell, so the
same check runs unchanged from build_and_test_linuxmac.sh and
build_and_test_windows.bat.
"""

import re
import sys
from pathlib import Path

JUCE_FREE_MODULES = (
    "jamn_core",
    "jamn_proto",
    "jamn_net",
    "jamn_dsp",
    "jamn_engine",
    "jamn_session",
)

PATTERN = re.compile(r"#include <juce|JuceHeader")


def find_violations(repo_root: Path):
    violations = []
    for module in JUCE_FREE_MODULES:
        module_dir = repo_root / "modules" / module
        if not module_dir.is_dir():
            continue
        for path in sorted(module_dir.rglob("*")):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                if PATTERN.search(line):
                    violations.append(f"{path}:{lineno}: {line.strip()}")
    return violations


def main():
    repo_root = Path(__file__).resolve().parent.parent
    violations = find_violations(repo_root)
    if violations:
        print("Module boundary violation: JUCE include found in a JUCE-free module:", file=sys.stderr)
        for violation in violations:
            print(violation, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
