# JamN

A low-latency networked jam room for 2-6 friends. Not a DAW - every feature
is judged by whether it helps people play music together right now.

## Install

Prerequisites:

- Python 3.
- CMake >= 3.25.1,
- Ninja
- Linux: g++, plus ALSA/freetype2/X11/Xrandr/Xinerama/Xcursor/Xext/GL dev
  packages (needed by JUCE).
- Windows: MSVC (Visual Studio Build Tools or full VS).
- `webkit2gtk` is not required - `JUCE_WEB_BROWSER`/`JUCE_USE_CURL` are off.

Clone this repo, then warm the shared dependency cache (Catch2/ENet/JUCE, fetched once
and reused across every build preset):

```bash
python3 scripts/bootstrap.py
```

Build and run every check:

```bash
./build_and_test_linuxmac.sh   # Linux/macOS (macOS untested)
build_and_test_windows.bat     # Windows (untested)
```

## Quick usage

```bash
./LAUNCH_JamN_linuxmac.sh      # Linux/macOS (macOS untested)
LAUNCH_JamN_windows.bat        # Windows (untested)
```

Runs the binary already built above. Pass `--headless` to exercise the audio
path with no window.

## Everything else

- **Contributing:** [`AGENTS.md`](AGENTS.md) is the single, complete
  rulebook - applies to humans and agents alike.
- **Architecture:** [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), with
  [`PROTOCOL.md`](docs/PROTOCOL.md), [`CLOCK.md`](docs/CLOCK.md),
  [`RT_RULES.md`](docs/RT_RULES.md) and
  [`MODULE_OWNERSHIP.md`](docs/MODULE_OWNERSHIP.md) alongside it.
- **License:** [MIT](LICENSE). Every dependency's own license is tracked in
  [`docs/LICENSES.md`](docs/LICENSES.md).
- **Design rationale:** the overall plan, phase breakdown and risk register
  live outside this repo, maintained privately. If a decision here needs
  justification you can't find in `docs/` or `AGENTS.md`, ask the
  maintainer rather than inferring it.
