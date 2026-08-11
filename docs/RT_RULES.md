# Real-time rules

These apply to anything that runs on the audio callback.

- No allocation. No `std::string` or `vector` growth. No allocating
  `std::function`. No `shared_ptr` copies.
- No locks of any kind. Not even `try_lock`.
- No I/O, no logging, no exceptions crossing the callback.
- All per-block work bounded at compile time.
- Exactly one `SpscRing` implementation exists and everything uses it.
  A new lock-free primitive requires an ADR (`docs/adr/`).

This is law. What follows is how it is enforced and what it looks like
in the actual code, not a relaxation of any of the above.

## `RealtimeScope` - the marker

`jamn::core::RealtimeScope`
(`modules/jamn_core/include/jamn_core/realtime_scope.h`) is an RAII
type whose constructor and destructor only ever touch a thread-local
`int`, so the marker itself never allocates, locks or logs. Its only
public surface is `IsActive()`.

Reporting goes through an indirection, `RealtimeViolationHandler`, so a
violation can be handled differently in production and in tests: the
default handler `fprintf`s to stderr and calls `std::abort()`; tests
install one that throws instead, so a violation can be asserted on
without killing the whole test binary.

## The allocation trap - where it actually lives

`RealtimeScope` on its own only tracks whether a real-time section is
active. The trap - global `operator new`/`operator delete` calling
`ReportRealtimeViolation()` when `RealtimeScope::IsActive()` is true -
lives in `modules/jamn_core/tests/realtime_allocation_guard.cpp`, an
OBJECT library linked only into `jamn_core_tests` and `jamn_dsp_tests`.
**Not** `jamn_bench_tests`, and not `jamn_app` itself. Overriding
global `operator new`/`delete` is a whole-binary decision that
production code should opt into deliberately, not inherit for free
from linking `realtime_scope.h`.

**A sanitizer lesson baked into that file.** Under `linux-asan`, our
own `operator new`/`delete` overrides have to win over ASan's own
replacements at link time - which means defining every overload
actually reachable from user code, sized and non-sized `delete` alike.
Defining only the non-sized `delete` let the sized overload fall
through to ASan's own replacement, which expects ASan-tracked memory:
every allocation was reported as an alloc-dealloc-mismatch, since it
had come from a plain `malloc`.

**The coverage gap, stated precisely.** The guard defines plain
`operator new`, `operator new[]`, and both sized and non-sized
`operator delete`/`delete[]`. It does **not** define the
`std::align_val_t` (over-aligned) or `std::nothrow_t` overloads. Code
that allocates over-aligned or with `std::nothrow` inside a
`RealtimeScope` will not trip the trap.

**Reentrancy.** A throwing violation handler allocates while
constructing its exception object (`std::runtime_error`'s message
storage, for instance). Without a guard against it, that allocation
would re-enter `operator new` while `RealtimeScope` is still active,
report a second violation, throw again constructing *that* exception,
and so on until the stack overflows. A thread-local
`g_reportingRealtimeViolation` flag plus a small RAII `ReentrancyGuard`
makes the nested allocation fall through to `malloc` unchecked instead.

## Why the guard runs over the real audio path on every `ctest`

`jamn::core::FileAudioDevice` drives an `AudioCallback` for a fixed
number of blocks the way a real device would, with no hardware and no
JUCE. Each call is wrapped in a `RealtimeScope`, so any test using it
exercises the allocation trap too, not just a single synthetic call.
Scratch space is allocated up front, outside the scope; interleaving
and the `fwrite` that records output for the test to read back also
happen outside the scope - the same way a real system hands output off
to a separate disk-writer thread rather than doing file I/O on the
audio thread itself.

Because `jamn_dsp` is JUCE-free and is driven by `FileAudioDevice` in
its tests, this guard runs over the real signal path - `JamAudio`
draining its trigger ring, `BlipVoice::Render`, `MasterBus::Process` -
every time `ctest -L fast` runs, with no audio hardware required.

## The `std::function` carve-out

`jamn::core::AudioCallback` (`file_audio_device.h`) **is** a
`std::function`, which looks like a direct contradiction of "no
allocating `std::function`." It isn't, and the reason is confinement,
not exception: `JuceAudioDevice::Open`
(`modules/jamn_platform/src/juce_audio_device.cpp`) is where
`callback_` gets assigned - `std::function` assignment is what
allocates, and `Open` runs once, off the audio thread, before any
callback is in flight. The real-time entry point,
`audioDeviceIOCallbackWithContext`, only ever *invokes* `callback_`,
and invoking an already-constructed `std::function` does not allocate.

## The compiler-elision lesson

A test that allocates and discards a value with no other observable
side effect is a legitimate target for compiler elision, not just a
sanitizer artifact - under `linux-tsan` at `-O1`, GCC once elided a
`new`/`delete` pair entirely because nothing used the pointee, so a
`RealtimeScope`-violation test passed by doing nothing. The fix was a
`volatile` sink that forces the allocation to be observed.

`modules/jamn_dsp/tests/jam_audio_tests.cpp`'s
`"JamAudio's real-time guard is actually armed in this binary"` test
case exists as the standing self-check for this: it deliberately
allocates inside a `RealtimeScope` with a `volatile int sink` forcing
the allocation to survive, and asserts the violation handler actually
fires. If this test ever silently passed by doing nothing, the guard
above would not be armed in that binary.

## Lifetime as an RT-safety concern

`jamn::platform::JuceAudioDevice`'s real-time entry point calls
`callback_(...)` directly, with no null check - so nothing may destroy
the object `callback_` was bound to while a callback could still be in
flight. `modules/jamn_app/src/main.cpp` states this as a standing
invariant: its `JamnApplication` declares `audio_` (the `JamAudio` the
callback is bound to) *before* `device_` (the `JuceAudioDevice` that
invokes it), so C++'s reverse-declaration-order destruction closes
`device_` - and with it, any in-flight callback - before `audio_` is
torn down. Reordering those two member declarations would silently
reopen a use-after-destruction window.

See `docs/MODULE_OWNERSHIP.md` for which modules these rules bind
(the six JUCE-free ones) and `docs/ARCHITECTURE.md` for the full
signal path these rules exist to protect.
