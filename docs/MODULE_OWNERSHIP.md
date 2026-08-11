# Module ownership

## The nine modules

```
modules/
  jamn_core       SpscRing, ByteReader/Writer, time types, clocks,
                  RealtimeScope, Histogram64
  jamn_proto      message structs, encode/decode, version negotiation,
                  fuzz target
  jamn_net        ITransport and its implementations
  jamn_dsp        IInstrument, ParamDescriptor, synths, drums, Strip,
                  MasterBus
  jamn_engine     ClockSync, AudioClock, TempoMap, EventScheduler,
                  JitterBuffer, MixerGraph
  jamn_session    roster, host-authority rules, join/leave state machine
  jamn_platform   JUCE: device I/O, keyboard/mouse input, MIDI, prefs,
                  WAV writer
  jamn_ui         JUCE GUI
  jamn_app        main(), wiring
```

This table states the eventual scope of each module, not what is built
today - see Status below for that. **The first six must never include
JUCE, touch a GUI, or open a device.** That is roughly 75% of the code,
and it is what makes the project testable by anyone, on their own
machine, with no GUI, no audio hardware and no second machine - this
project has no CI, so that has to be true locally or it is not true
anywhere.

`jamn_core` depends on nothing. Exact edges among the other modules are
fixed by CMake as they are built; adding a new edge needs a reason
stated in the PR.

Prefer one task per module per PR. The module split is what lets two
people work at once without colliding.

## Status, as of `340e43c`

| Module | Status | What actually exists |
|---|---|---|
| `jamn_core` | Real | `SpscRing`, `RealtimeScope`, `FileAudioDevice`, `AudioCallback`. No dependencies, links no JUCE. |
| `jamn_dsp` | Real | `BlipVoice`, `MasterBus`, `JamAudio`. Depends on `jamn_core` only. |
| `jamn_platform` | Thin, real | `JuceAudioDevice` only. No WAV writer, no MIDI, no prefs, no keyboard/mouse input yet, despite the table above. |
| `jamn_ui` | Thin, real | `JamWindowContent` only - one button, one slider. |
| `jamn_app` | Real | `main()` and the wiring: `RunHeadless`, `MainWindow`, `JamnApplication`. |
| `tools/jamn_bench` | Real, small | `BenchResult`, `ToJson`, one backend (`file-audio-device`). Not a module under `modules/`, but follows the same JUCE-free rule by construction - it links `jamn_core` only. |
| `jamn_proto` | Stub | One 5-line header, `namespace jamn::proto {}`. No message types, no encode/decode. |
| `jamn_net` | Stub | One 5-line header, `namespace jamn::net {}`. No `ITransport`. |
| `jamn_engine` | Stub | One 5-line header naming its eventual scope in a comment. |
| `jamn_session` | Stub | One 5-line header naming its eventual scope in a comment. |

A stub module is `add_library(<name> INTERFACE)` plus an include
directory - no sources, no `target_link_libraries`, no tests.

## The dependency graph, as CMake actually encodes it today

```
jamn_core     <- (nothing)
jamn_dsp      -> jamn_core
jamn_platform -> jamn_core, juce::juce_audio_devices
jamn_ui       -> juce::juce_gui_basics
jamn_app      -> jamn_core, jamn_dsp, jamn_engine, jamn_session,
                 jamn_platform, jamn_ui, juce::juce_core,
                 juce::juce_gui_basics
jamn_bench    -> jamn_bench_lib, jamn_core
jamn_proto, jamn_net, jamn_engine, jamn_session -> (nothing)
```

Two things worth calling out because they are easy to miss reading the
table at the top of this file instead of the CMake:

- `jamn_app` links `jamn_engine` and `jamn_session` even though both
  are still stubs contributing nothing - the edge exists for when they
  stop being stubs, not because `jamn_app` uses them today.
- `jamn_ui` deliberately has no `jamn_dsp` edge.
  `modules/jamn_ui/CMakeLists.txt` says why: `JamWindowContent` reports
  what the user did through two `std::function` outputs and knows
  nothing about what happens next.

## What the boundary checker actually enforces

`tools/check_module_boundaries.py` is one rule: no line matching
`#include <juce|JuceHeader` may appear anywhere under
`modules/jamn_core`, `jamn_proto`, `jamn_net`, `jamn_dsp`, `jamn_engine`
or `jamn_session`. It walks every file in each module directory, not
only `.h`/`.cpp`.

Two limits worth stating plainly, since the rule is easy to over-trust:

- It is a **text match, including inside comments** - a commented-out
  `#include <juce_core/...>` would still fail it.
- It does **not** enforce the dependency graph above. Nothing stops
  `jamn_core` from `#include`-ing something under `jamn_dsp`; the
  edges in that graph are a product of `target_link_libraries` and
  include-path visibility, not of this script.

It does not cover `tools/`. Both gate scripts
(`build_and_test_linuxmac.sh`, `build_and_test_windows.bat`) run it as
their first step.

## Test coverage by module

`jamn_core` and `jamn_dsp` have their own Catch2 test binaries
(`jamn_core_tests`, `jamn_dsp_tests`, both labelled `fast`).
`jamn_platform` and `jamn_ui` have **no test target of their own** -
they are exercised only transitively, through `jamn_app_smoke`
(labelled `app`, links JUCE). `jamn_proto`, `jamn_net`, `jamn_engine`
and `jamn_session` have nothing to test yet.

See `docs/RT_RULES.md` for what runs inside those `fast` tests, and
`docs/ARCHITECTURE.md` for how the preset/label split works.
