# AGENTS.md

A jam room for 2-6 friends. Not a DAW. Every feature is judged by whether it
helps people play music together right now.

This is the single rulebook for this repo. It applies to everyone, human or
agent, regardless of which tool or model you are. `CLAUDE.md` is a stub that
points here. There is no second rulebook, and nothing here assumes any
particular assistant, editor or CLI.

## Status: Phase 0 in progress

This repo is deliberately incomplete - modules, docs and tools referenced
below get built out over time, so reaching for one that's missing isn't a
broken checkout. Linux (`core-only`, `linux-ninja`) is verified green;
`windows-msvc` is unverified for lack of a Windows machine. The authoritative
list of what's actually still open is maintainer-private, so ask the
maintainer rather than assuming a missing piece is a bug.

Everything else in this file, and in `docs/MODULE_OWNERSHIP.md`,
`docs/RT_RULES.md` and `docs/PROTOCOL.md`, is in force regardless.

## Fast path

Run `./build_and_test_linuxmac.sh` (Linux, and untested-but-expected-to-work
unchanged on macOS) or `build_and_test_windows.bat` (Windows) before opening
a PR. This is the definition of "passing", and it is the only thing that
should be run between changes - not a hand-picked subset of its steps. Read
the script for the exact sequence rather than trusting a summary of it here.
As new test types get added - fuzzing, golden-vector compares, sim/render/
loopback, protocol-doc checks, whatever comes next - they get wired into the
same script, not run separately by hand or enumerated in this file.

Dependencies are fetched by CMake at configure time, pinned by tag and commit
SHA. `python3 scripts/bootstrap.py` warms that cache and is safe to re-run: on
a warm cache it makes no network request at all. If you are working somewhere
without network access during the editing phase, run it first.

## Modules

Six of the nine modules must never include JUCE, touch a GUI, or open
a device - that split is what makes the project testable by anyone, on
their own machine, with no GUI, no audio hardware and no second
machine, which matters because this project has no CI. See
`docs/MODULE_OWNERSHIP.md` for the module table, which six, current
status of each, the real dependency graph, and exactly what the
JUCE-boundary checker enforces.

## Real-time rules

These apply to anything that runs on the audio callback. See
`docs/RT_RULES.md` for the rules themselves and how they're enforced
mechanically.

## Protocol rules

These are law - they exist so that two peers built from different
commits do not silently corrupt each other. See `docs/PROTOCOL.md` for
the rules themselves and the wire format they govern.

## Definition of done

This project has no CI and none is planned - there is no pipeline to catch
what a human skipped, so everything below is run manually, and whoever
reviews is the arbiter: actually run the scripts and read their output, not
a summary of them. A human merges; do not auto-merge.

A PR is ready when all of these are true.

- `./build_and_test_linuxmac.sh` (or `build_and_test_windows.bat` on
  Windows) is green - the entire verification gate.
- If the change touches concurrency-sensitive or allocation-sensitive code
  (`jamn_core`, anything bound by `docs/RT_RULES.md`, `SpscRing`/
  `RealtimeScope` themselves): also manually run the sanitizer presets -
  `cmake --preset linux-asan|linux-tsan && ninja -C build-asan|build-tsan
  && ctest --preset linux-asan|linux-tsan -L fast`. Both currently cover
  only the core-only scope, not the JUCE-linking build.
- If the protocol changed: `docs/PROTOCOL.md` and the golden vectors are
  updated in the same PR.
- If a dependency was added: `third_party/<name>/` has both a LICENSE and a
  PROVENANCE.md.
- If behaviour changed in a way a user or builder would notice: `CHANGELOG.md`
  has an entry. Scaffolding and no-op refactors do not get one.
- The PR description says plainly which agent or model wrote it, if any. A
  one-line "Generated with X" is enough. This is about accurate attribution,
  nothing more.

## Where the design reasoning lives

The overall plan, architecture rationale, risk register and the list of what
is still open live in a private maintainer repo, not this one. That is
deliberate: everything you need in order to work here belongs in `docs/` and
in this file. If a task needs justification you cannot find in the repo, or
you want to know what is still open, ask the maintainer rather than inferring
it or guessing at intent.

`CHANGELOG.md` explains its own scope, versioning scheme and conventions at
the top - read it there rather than here.

## Out of scope

Named explicitly, because each is bottomless and each will look reasonable in
isolation: drag-to-dock windows, a general audio graph with arbitrary routing,
plugin delay compensation, and NAT traversal or relay infrastructure. A PR
that adds a graph node type is a warning sign.
