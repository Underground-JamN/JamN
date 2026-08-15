#!/usr/bin/env python3
"""Checks that jamn_proto's MessageType enum and docs/PROTOCOL.md's
message-type table never drift apart, on name, value and build status. Each
carries all three independently (the enum is what the compiler enforces, the
table is what an outside contributor reads without opening jamn_proto's
headers) - see docs/PROTOCOL.md's "Message types" section and its own comment
on message_type.h. Python, not bash/PowerShell, so it runs unchanged from
build_and_test_linuxmac.sh and build_and_test_windows.bat.
"""

import re
import sys
from pathlib import Path

ENUM_PATH = "modules/jamn_proto/include/jamn_proto/message_type.h"
DOC_PATH = "docs/PROTOCOL.md"

STATUSES = ("Built", "Reserved")

# "kNoteBurst = 9,          // Built - channel 1 Realtime." - the leading "k"
# is this codebase's enumerator-naming convention (see jamn_dsp's
# TriggerEvent), stripped to get the plain name the doc table uses. The
# trailing comment's leading keyword is the status; only that keyword is
# compared, since the two sides legitimately elaborate differently
# ("Built (Hello)" in the doc vs "Built (Hello) - channel 0 Control." here).
ENUM_ENTRY = re.compile(
    r"\bk([A-Za-z][A-Za-z0-9]*)\s*=\s*(\d+)\s*,\s*//\s*(" + "|".join(STATUSES) + r")\b"
)

# The same, without requiring the status comment - so a missing status is
# reported as a missing status rather than silently dropping the whole row
# out of the comparison.
ENUM_ENTRY_ANY = re.compile(r"\bk([A-Za-z][A-Za-z0-9]*)\s*=\s*(\d+)\s*,")

# "| Join | 1 | 0 Control | Built (`Hello`) |" - name, value, channel,
# status. The channel column is captured but not compared: the enum's own
# comment carries it as free text, not as a field this can diff reliably.
TABLE_ROW = re.compile(
    r"^\|\s*([A-Za-z][A-Za-z0-9]*)\s*\|\s*(\d+)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|"
)


def leading_status(cell):
    """The Status column's leading keyword, or None if it isn't one."""
    match = re.match(r"(" + "|".join(STATUSES) + r")\b", cell.strip())
    return match.group(1) if match else None


def parse_enum(text):
    """{name: (value, status)}. status is None when the enumerator carries no
    recognizable Built/Reserved comment."""
    with_status = {name: (int(value), status) for name, value, status in ENUM_ENTRY.findall(text)}
    entries = {}
    for name, value in ENUM_ENTRY_ANY.findall(text):
        entries[name] = with_status.get(name, (int(value), None))
    return entries


def parse_table(text):
    """{name: (value, status)}. status is None when the Status cell doesn't
    start with a recognizable keyword."""
    entries = {}
    in_section = False
    for line in text.splitlines():
        if line.strip() == "## Message types":
            in_section = True
            continue
        if in_section and line.startswith("## "):
            break
        if not in_section:
            continue
        match = TABLE_ROW.match(line.strip())
        if match:
            name, value, status_cell = match.group(1), int(match.group(2)), match.group(4)
            if name == "Name":  # the header row itself
                continue
            entries[name] = (value, leading_status(status_cell))
    return entries


def main():
    repo_root = Path(__file__).resolve().parent.parent
    enum_text = (repo_root / ENUM_PATH).read_text(encoding="utf-8")
    doc_text = (repo_root / DOC_PATH).read_text(encoding="utf-8")

    enum_entries = parse_enum(enum_text)
    doc_entries = parse_table(doc_text)

    if not enum_entries:
        print(f"No MessageType entries found in {ENUM_PATH} - check ENUM_ENTRY's pattern", file=sys.stderr)
        return 1
    if not doc_entries:
        print(f"No message-type table rows found in {DOC_PATH}'s '## Message types' section", file=sys.stderr)
        return 1

    problems = []
    for name, (value, status) in sorted(enum_entries.items()):
        if status is None:
            problems.append(
                f"{name} in {ENUM_PATH} has no '// Built'/'// Reserved' status comment"
            )
        if name not in doc_entries:
            problems.append(f"{name} (value {value}) is in {ENUM_PATH} but missing from {DOC_PATH}'s table")
            continue
        doc_value, doc_status = doc_entries[name]
        if doc_value != value:
            problems.append(f"{name} is value {value} in {ENUM_PATH} but {doc_value} in {DOC_PATH}'s table")
        if doc_status is None:
            problems.append(
                f"{name}'s Status column in {DOC_PATH} does not start with "
                f"{' or '.join(STATUSES)}"
            )
        elif status is not None and doc_status != status:
            problems.append(
                f"{name} is '{status}' in {ENUM_PATH} but '{doc_status}' in {DOC_PATH}'s table"
            )
    for name, (value, _status) in sorted(doc_entries.items()):
        if name not in enum_entries:
            problems.append(f"{name} (value {value}) is in {DOC_PATH}'s table but missing from {ENUM_PATH}")

    if problems:
        print("Protocol doc / MessageType enum mismatch:", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
