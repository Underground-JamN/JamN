# Architecture

A jam room for 2-6 friends. This file is the entry point into how the
system is put together; the other docs it links to each own one
subject in depth, so read this one first.

## What runs end to end today

The two paths now connect. Phase 0's audio path reaches real JUCE
device I/O; Phase 0.5a's timing/network core reaches a real socket; and
`jamn_engine`'s `AudioRuntime` joins them, draining the note crossing at
block start, converting each remote note out of its sender's timebase,
scheduling it, and handing what is due back to `jamn_app` to play
through a peer strip's instrument. Note placement is quantised to one
audio block for now.

Not yet real: nothing plays *local* input (there is no input path until
Wave 6), and the GUI shows none of this. This page still describes the
Phase 0 audio path in the detail below; bringing the rest of it current
is Phase 0.5b's closing task.

Phase 0's audio signal path:

```
JamWindowContent's blip button
  -> JamAudio::Trigger()               (message thread)
  -> TriggerRing                       (SpscRing<TriggerEvent, 16>)
  -> JamAudio::Process()               (audio thread, drains the ring)
  -> BlipVoice::Render()
  -> PeerMixer::Render()               (kMaxPeers strips, summed)
  -> MasterBus::Process()              (master gain, then the limiter)
  -> JuceAudioDevice's real-time callback
```

Nothing before `MasterBus` is bounded: `PeerMixer` sums its strips and
lets the total go where it goes, so the limiter is the single place the
signal is held to full scale. It has no lookahead, deliberately -
`docs/CLOCK.md` makes "a player's own local input is always monitored
live, never delayed" non-negotiable, and a lookahead delay line on the
master is precisely that delay. Without lookahead a smoothed envelope
cannot catch the first sample of a transient, so a hard clamp behind it
is what actually guarantees the ceiling, and the envelope is what stops
that clamp working continuously on a sustained loud mix.

`JamAudio` (`modules/jamn_dsp/include/jamn_dsp/jam_audio.h`) is
explicitly one local voice plus a fixed array of `kMaxPeers` peer
strips into one master gain, all fixed at compile time - not a graph,
and it must not grow into one. The array is the whole of the peer
mixing model: a peer joining repoints a slot's instrument, it never
adds a node. `BlipVoice` sits outside that array on purpose - it is
the local "prove the device works" blip, not a peer, so another peer's
solo must not silence it. `docs/RT_RULES.md` covers the
real-time constraints on everything from `JamAudio::Process` down;
`docs/MODULE_OWNERSHIP.md` covers what each module in this chain owns
and what it doesn't yet.

Phase 0.5a's timing/network core, sim-only and headless (no ENet, no
audio device, no GUI - see `docs/PROTOCOL.md` and `docs/CLOCK.md`):

```
BurstAssembler                          (jamn_engine, K=3 redundancy)
  -> jamn_proto encode/decode           (PacketHeader + TLV framing)
  -> ITransport                         (SimTransport in 0.5a; EnetTransport in 0.5b)
  -> DedupeRing                         (jamn_engine, receiver side)
  -> ClockSync / JitterBuffer / EventScheduler
```

`EventScheduler`'s `IDeadlineResolver` seam and `ClockSync`'s
`SessionTime` offset model are the pieces `AudioClock` and the real
audio callback will connect into in 0.5b - see the "designed, not
built" status still standing in `docs/CLOCK.md` for exactly what
remains.

## Threads, and the one crossing point that exists so far

This section describes the Phase 0 audio path specifically. The
engine path now has a second crossing of its own: `NoteCrossing`, one
`SpscRing` lane per peer, published by the net thread and drained by the
audio thread inside `AudioRuntime::Service`. `EventScheduler` and
`AudioClock` run there, on the audio thread, at block start - not on
whatever thread a test happens to call them from.

Phase 0's audio path lives on one of two threads: the message thread
(button clicks, slider drags, anything JUCE delivers as a UI event)
and the audio thread (the real-time callback). The only place they
cross in the code that exists today is `TriggerRing`, a single
`SpscRing` instance - `JamAudio::Trigger()` pushes from the message
thread, `JamAudio::Process()` pops from the audio thread, and nothing
else touches that ring. Gain, mixer controls and device state cross the
same boundary through plain atomics (`MasterBus`'s gain target, each
`Strip`'s volume/mute/solo and its non-owning instrument pointer,
`JuceAudioDevice`'s `sampleRate_`/`blockSize_`), never through a lock.

`docs/RT_RULES.md` has the full real-time rule set and the mechanism
(`RealtimeScope` plus a test-only allocation trap) that catches a
violation of it.

A third thread is now real: `jamn_engine`'s `NetThread` drives
`PeerRuntime::Service` whenever `jamn_app` was given `--listen` or
`--connect`. It does not yet cross into the audio thread - a received
note reaches `NoteCrossing` and waits there, because nothing drains it
from the callback yet. When that wiring lands, `NoteCrossing` becomes
the second cross-thread boundary in this file alongside `TriggerRing`,
and this section covers it then.

## Build topology

Five CMake presets, all Ninja, all C++20:

| Preset | Binary dir | Scope |
|---|---|---|
| `core-only` | `build-core-only/` | `JAMN_CORE_ONLY=ON` - the six JUCE-free modules only |
| `linux-ninja` | `build/` | Full build, JUCE included |
| `windows-msvc` | `build-windows-msvc/` | Full build, unverified - no Windows machine available yet |
| `linux-asan` | `build-asan/` | Core-only scope, `-fsanitize=address,undefined` |
| `linux-tsan` | `build-tsan/` | Core-only scope, `-fsanitize=thread` |

Both sanitizer presets are deliberately core-only: they never build
JUCE, so they run fast enough to use routinely and never need to catch
issues in code outside their own scope.

Two ctest labels exist: `fast` (the JUCE-free suite - `jamn_core_tests`,
`jamn_proto_tests`, `jamn_net_tests`, `jamn_dsp_tests`,
`jamn_engine_tests`, `jamn_bench_tests` - which must run identically
under `core-only` and the full build, and must never link JUCE) and
`app` (`jamn_app_smoke`, which runs the real `jamn_app --headless`
binary and links JUCE). `./build_and_test_linuxmac.sh` /
`build_and_test_windows.bat` run the module-boundary check, both
`fast` suites, and `app`, in that order - see those scripts for the
exact sequence rather than trusting a summary of it here, per
`AGENTS.md`'s Fast path.

## Where each subject is documented

- `docs/MODULE_OWNERSHIP.md` - the nine modules, what each actually
  contains today versus its eventual scope, the real CMake dependency
  graph, and what the JUCE-boundary checker does and doesn't enforce.
- `docs/RT_RULES.md` - the real-time rules, `RealtimeScope`, the
  allocation trap, and the lifetime/ordering rules that protect the
  audio callback.
- `docs/PROTOCOL.md` - the wire protocol's compatibility rules (law
  today), its packet/message-type format (built, in `jamn_proto`), and
  ENet channel assignment (still unbuilt - `EnetTransport` is 0.5b
  scope).
- `docs/CLOCK.md` - the timebase design: the three time types, peer
  clock synchronization, and the scheduler (all built, in `jamn_core`
  and `jamn_engine`) - `AudioClock` alone is still designed, not
  built, until 0.5b.
