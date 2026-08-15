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
                  PeerMixer, MasterBus
  jamn_engine     ClockSync, AudioClock, AudioRuntime, TempoMap, EventScheduler,
                  JitterBuffer, BurstAssembler, DedupeRing
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

## Status, as of Phase 0.5b T5.0

| Module | Status | What actually exists |
|---|---|---|
| `jamn_core` | Real | `SpscRing`, `RealtimeScope`, `FileAudioDevice`, `AudioCallback`, time types, `IClock`/`SteadyClock`/`SimClock`, `ByteReader`/`ByteWriter`, `Histogram64`. No dependencies, links no JUCE. |
| `jamn_dsp` | Real | `BlipVoice`, `MasterBus`, `JamAudio`, `IInstrument`/`TestToneInstrument`, `Strip`, `PeerMixer`, and the shared `gain_ramp.h`. `MasterBus` is gain plus a peak limiter (no lookahead - `docs/CLOCK.md` forbids delaying local monitoring - so a hard clamp guarantees the ceiling and the envelope keeps it from working continuously). Depends on `jamn_core` only. |
| `jamn_platform` | Thin, real | `JuceAudioDevice` only. No WAV writer, no MIDI, no prefs, no keyboard/mouse input yet, despite the table above. |
| `jamn_ui` | Thin, real | `JamWindowContent` only - one button, one slider. |
| `jamn_app` | Real | `main()` and the wiring: `RunHeadless`, `MainWindow`, `JamnApplication`. `--listen <port>` / `--connect <host:port>` bring a session up; with `--headless` they run a bounded, self-scoring net session whose verdict is the exit code. Listening binds 127.0.0.1 only for now - widening it belongs with the join-by-`ip:port` dialog. |
| `tools/jamn_bench` | Real, small | `BenchResult`, `ToJson`, `FromAudioBlockTiming`, and three backends (`file-audio-device`, `event-scheduler`, `loopback-clock`). Not a module under `modules/`, but follows the same JUCE-free rule by construction - it links `jamn_core` and `jamn_engine`. The schema lives in `jamn_bench_lib`, split from the executable so `jamn_app` can link it and emit the same rows from a real device (`--bench-json`) without any of it reaching the `core-only` preset. |
| `jamn_proto` | Real | `PacketHeader`, TLV framing, `MessageType`, `NoteEvent`/`NoteBurst`/`Hello`/`SessionConfig`/`InstrumentAssign`, golden vectors, a fuzz-replay corpus, join authentication (`ConstantTimeEquals`, `DecodePacketAuthenticated`). Depends on `jamn_core` only - no ENet, no JUCE. |
| `jamn_net` | Real | `ITransport`, `SimTransport`, `EnetTransport` (one real UDP socket, `docs/PROTOCOL.md`'s channel table mapped onto ENet's packet flags). Depends on `jamn_core` and ENet. |
| `jamn_engine` | Real (except `TempoMap`) | `ClockSync`, `JitterBuffer`, `EventScheduler` (+ `IDeadlineResolver`/`LiveResolver`/`MusicalResolver`), `BurstAssembler`, `DedupeRing`, and `PeerRuntime` - the production owner of the net thread (transport poll loop, clock ping cadence and pong replies, burst assembly and broadcast, receive -> dedupe -> hand off). `PeerRuntime` is session-agnostic: it hands control-channel packets out whole through a callback rather than linking `jamn_session`, which is what keeps that edge absent in both directions. `NoteCrossing` is the single exit for a received note - one `SpscRing` lane per peer slot, published by the net thread and drained by the audio thread, reusing the one sanctioned lock-free primitive rather than introducing a new one (so no ADR, per `docs/RT_RULES.md`). `NetThread` is the thread itself: it drives `Service` on an absolute 250us schedule (`docs/CLOCK.md` says why that number), joins on `Stop()`, and reports the interval it achieved rather than the one it requested. It lives here rather than in `jamn_app` so `ctest -L fast` and both sanitizer presets - all core-only scope - can see it. `AudioClock` is Clock 2 - a second-order DLL measuring the device's real sample rate against the steady clock, built and unit-tested but never yet fed by a real device. `AudioRuntime` is the audio thread's counterpart to `PeerRuntime`: one `Service` call at block start drives `AudioClock`, drains every `NoteCrossing` lane, converts each note out of its sender's timebase, schedules it, and hands back what is due. It stops at data rather than making sound, so that no `jamn_dsp` edge is needed in either direction - `jamn_app` links both and does the instrument calls. `TempoMap` is later-phase scope with nothing to build against yet. Depends on `jamn_core`, `jamn_proto`, `jamn_net`, and `Threads::Threads`. |
| `jamn_session` | Real | `Roster` (fixed-capacity, per-peer handshaking -> joined -> leaving state machine) and `SessionHost` (join authority: constant-time passphrase comparison, `proto_major` refused, `proto_minor` negotiated down). `SessionHost` deliberately does not install itself as the transport's callback - it exposes `HandlePeerEvent`/`HandleControlPacket` for the caller to drive, so T4.1's runtime stays the single owner of those callbacks. Depends on `jamn_net`, `jamn_proto`. |

A stub module is `add_library(<name> INTERFACE)` plus an include
directory - no sources, no `target_link_libraries`, no tests.

## The dependency graph, as CMake actually encodes it today

```
jamn_core     <- (nothing)
jamn_proto    -> jamn_core
jamn_net      -> jamn_core (+ enet, PRIVATE and conditional)
jamn_dsp      -> jamn_core
jamn_engine   -> jamn_core, jamn_proto, jamn_net
jamn_session  -> jamn_net, jamn_proto
jamn_platform -> jamn_core, juce::juce_audio_devices
jamn_ui       -> juce::juce_gui_basics
jamn_app      -> jamn_core, jamn_dsp, jamn_engine, jamn_session,
                 jamn_platform, jamn_ui, jamn_bench_lib,
                 juce::juce_core, juce::juce_gui_basics
jamn_bench_lib -> jamn_core
jamn_bench    -> jamn_bench_lib, jamn_core, jamn_engine
```

Two things worth calling out because they are easy to miss reading the
table at the top of this file instead of the CMake:

- `jamn_app` now calls into `jamn_engine`: given `--listen` or
  `--connect` it opens an `EnetTransport`, builds a `PeerRuntime` and
  starts a `NetThread` on it. The `jamn_session` edge is still
  forward-looking - nothing runs the join handshake yet. And the two
  halves of the program still do not meet: nothing carries a received
  note across to the audio callback, so a remote note reaches
  `NoteCrossing` and stops there. The same is true one level down
  inside `jamn_dsp`: `PeerMixer`'s strips all hold a null instrument
  until something assigns peers to them.
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

`jamn_core`, `jamn_proto`, `jamn_net`, `jamn_dsp` and `jamn_engine`
each have their own Catch2 test binary (`jamn_core_tests`,
`jamn_proto_tests`, `jamn_net_tests`, `jamn_dsp_tests`,
`jamn_engine_tests`, all labelled `fast`). `jamn_platform` and
`jamn_ui` have **no test target of their own** - they are exercised
only transitively, through `jamn_app_smoke` (labelled `app`, links
JUCE). `jamn_session` has its own `jamn_session_tests` too, also
`fast`; `jamn_net`'s ENet-linking cases are a separate binary under
the `net` label, since `fast` must be identical under `core-only`.

Two `net` cases are not Catch2 binaries at all. `jamn_loopback_peer`
plus `run_two_process_loopback.py` (both under
`modules/jamn_engine/tests/`) run two real processes against each other
on 127.0.0.1; `run_two_process_app.py` (under
`modules/jamn_app/tests/`) does the same with two
`jamn_app --headless` processes, which is what covers the app's own net
thread and its shutdown ordering. In both, the assertions live in the
processes' exit codes rather than in a test framework, because what is
under test is two processes that share no memory agreeing - which is
exactly what a single in-process framework cannot express. The second
carries `net` rather than `app` despite linking JUCE: both labels run
only in the full build, and binding a socket is the property that
decides whether it can run at all.

See `docs/RT_RULES.md` for what runs inside those `fast` tests, and
`docs/ARCHITECTURE.md` for how the preset/label split works.
