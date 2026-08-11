# Architecture

A jam room for 2-6 friends. This file is the entry point into how the
system is put together; the other docs it links to each own one
subject in depth, so read this one first.

## What runs end to end today

Phase 0's whole signal path, and currently the only complete path in
the codebase:

```
JamWindowContent's blip button
  -> JamAudio::Trigger()               (message thread)
  -> TriggerRing                       (SpscRing<TriggerEvent, 16>)
  -> JamAudio::Process()               (audio thread, drains the ring)
  -> BlipVoice::Render()
  -> MasterBus::Process()
  -> JuceAudioDevice's real-time callback
```

`JamAudio` (`modules/jamn_dsp/include/jamn_dsp/jam_audio.h`) is
explicitly one voice into one master gain, fixed at compile time - not
a graph, and it must not grow into one. `docs/RT_RULES.md` covers the
real-time constraints on everything from `JamAudio::Process` down;
`docs/MODULE_OWNERSHIP.md` covers what each module in this chain owns
and what it doesn't yet.

## Two threads, one crossing point

Everything above lives on one of two threads: the message thread
(button clicks, slider drags, anything JUCE delivers as a UI event)
and the audio thread (the real-time callback). The only place they
cross in the code that exists today is `TriggerRing`, a single
`SpscRing` instance - `JamAudio::Trigger()` pushes from the message
thread, `JamAudio::Process()` pops from the audio thread, and nothing
else touches that ring. Gain and device state cross the same boundary
through plain atomics (`MasterBus`'s gain target,
`JuceAudioDevice`'s `sampleRate_`/`blockSize_`), never through a lock.

`docs/RT_RULES.md` has the full real-time rule set and the mechanism
(`RealtimeScope` plus a test-only allocation trap) that catches a
violation of it.

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
`jamn_dsp_tests`, `jamn_bench_tests` - which must run identically
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
  today) and its designed-not-built format (ENet channels, packet
  shape, note representation).
- `docs/CLOCK.md` - the timebase design: the three time types, peer
  clock synchronization, and the scheduler - also designed, not built.
