# Changelog

Notable changes to JamN, newest first. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), except versioning:
`MAJOR.MINOR` only, no patch number. This project has no CI and cuts
releases by hand rather than continuously, so the thing a patch number
usually buys - "bugfix only, safe to upgrade blindly" - doesn't apply here;
every release, fix or feature alike, bumps the minor. `1.0` is reserved for
whenever the wire protocol and module APIs actually stabilize.

This file is the counterpart to the maintainer's backlog, which is private
(see `AGENTS.md`'s "Where the design reasoning lives"). A task is deleted
from that backlog when it is done rather than being rewritten as a "landed"
note, so this is where completed work is recorded and stays recorded.

What belongs here: changes someone using or building JamN would notice.
New instruments, protocol changes, build or dependency changes, fixed bugs,
anything that changes how the app behaves or how you compile it.

What does not: routine scaffolding, doc tweaks, refactors with no observable
effect, and work in progress. Git history already covers those, and a
changelog padded with them stops being read.

Protocol changes get an entry every time, including the `proto_major` /
`proto_minor` numbers involved, because peers on different versions have to
be diagnosable from this file alone.

Each entry is one or two lines: what changed, for a human skimming the
file. Verification detail and rationale belong in the commit itself (git
history already keeps it); if an entry would otherwise restate a doc's
content, link the doc instead of summarizing it.

## [Unreleased]

## [0.1] - 2026-08-10

### Added

- Architecture docs under `docs/`: `ARCHITECTURE.md`, `MODULE_OWNERSHIP.md`,
  `RT_RULES.md`, `PROTOCOL.md`, `CLOCK.md`, and `docs/adr/README.md` for the
  ADR format. `AGENTS.md`'s Modules/Real-time/Protocol sections now just
  point at them.
- Phase 0's window: `jamn_app` opens with a button (plays a blip) and a
  slider (drives master gain), backed by a real JUCE audio device
  (`jamn_platform`'s `JuceAudioDevice`).
- `jamn_app --headless` runs the real signal path with no window or device
  requirement; `--headless --out <path>` dumps it as raw PCM.
- `jamn_dsp` is real: `MasterBus` (ramped gain), `BlipVoice` (a synthesised
  blip) and `JamAudio` (Phase 0's signal chain), covered by RT-safety-checked
  tests.
- Licensed MIT (`LICENSE`); every dependency's license is tracked in
  `docs/LICENSES.md`.
- Repo is owned and published by the `Underground-JamN` GitHub org, recorded
  in `LICENSE` and `docs/LICENSES.md`.
- `build_and_test_linuxmac.sh`/`.bat`: the one command to run between
  changes. Added a `windows-msvc` preset and a shared
  `tools/check_module_boundaries.py`.
- `linux-asan`/`linux-tsan` presets (core-only scope) - found and fixed two
  real allocation bugs along the way.

### Changed

- `ctest -L fast` is JUCE-free again and identical across every preset;
  `jamn_app_smoke` moved from the `fast` label to `app`.
- This project has no CI, and none is planned. `build_and_test_linuxmac.sh`/
  `.bat`, run manually, are the entire verification gate.
