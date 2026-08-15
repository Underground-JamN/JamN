# Clock and timebase

## Status: built, but never driven by a real device

Everything on this page is built: the three time types, `ClockSync`
(Clock 1) and the one-scheduler-two-resolvers design as of Phase 0.5a,
`AudioClock` (Clock 2) since. All of it is real, tested code in
`jamn_core` and `jamn_engine` - but all of it has been exercised in
virtual or synthetic time only, never against a real audio device or a
GUI (see `docs/PROTOCOL.md`'s scope fence). ENet is no longer on that
list: Clock 1's exchange runs over a real socket as of 0.5b.
Clock 2 (`AudioClock`) is built and unit-tested against synthetic
callback streams, but has never been fed by a real audio device - that
last step, and the scheduler wiring that consumes it, is 0.5b's
remaining work.
`modules/jamn_platform/src/juce_audio_device.cpp`'s real-time callback
now accumulates the `(cumulative samples, steady-clock time at callback
entry)` pair Clock 2 takes as input, and reports it through
`JuceAudioDevice::TakeBlockTiming`.

**There is no hardware timestamp to use on this platform**, which an
earlier version of this page claimed there was. JUCE's
`juce::AudioIODeviceCallbackContext` is exactly
`{ const uint64_t* hostTimeNs = nullptr; }` in 8.0.9, and only the
CoreAudio backend ever populates it - so on Linux and Windows the
parameter carries nothing, `steady_clock` read at callback entry is the
primary source, and the callback still passes the context to
`juce::ignoreUnused`. It is read in nanoseconds rather than the
microseconds used everywhere else on this page: a block is 2667us at
128 frames and 48kHz, so microsecond rounding would inject roughly
190ppm of quantisation per callback into a rate estimate whose entire
job is resolving drift measured in ppm.

## Three time types, never interchangeable

Built: `modules/jamn_core/include/jamn_core/time_types.h`. Each is a
distinct wrapper type with no implicit conversion to any other, or to
plain `int64_t`, in either direction - enforced by `static_assert`, not
just a comment.

| Type | Unit | Scope |
|---|---|---|
| `SessionTime` | `int64` microseconds since session epoch | shared - the only "when" on the wire |
| `SampleTime` | `int64` samples at the local device rate | local only - peers run different device rates |
| `MusicalTime` | `int64` PPQ ticks at 960 PPQ, absolute and ever-increasing | shared, derived through a tempo map |

Microseconds, specifically - not samples, because peers run different
device rates (44.1/48/96 kHz); not floats, for precision loss and
nondeterminism; not milliseconds, too coarse for a low-single-digit-ms
budget. `MusicalTime` exists as a type today, but nothing produces one
yet - the tempo map it's derived through is Phase 2b scope.

## Clock 1: peer offset

Built: `modules/jamn_engine/include/jamn_engine/clock_sync.h`
(`ClockSync`). NTP-style exchange over the same UDP socket used for
everything else - in 0.5a this runs entirely over `SimTransport`
(`docs/PROTOCOL.md`'s scope fence); the real ENet-backed exchange is
0.5b.

```
rtt    = (t4-t1) - (t3-t2)
offset = ((t2-t1) + (t3-t4)) / 2
```

- **Minimum-RTT selection, not averaging.** The lowest-RTT sample in a
  rolling window is the least contaminated by queueing delay.
  Averaging is worse - one delayed packet drags the mean.
- **Model skew, don't just filter it.** A least-squares fit of offset
  against local time gives a drift rate in ppm. Consumer crystal drift
  is large enough over a long session to dominate everything else if
  left unmodeled.
- **Lock, then slew - never step.** The offset is hard-set once enough
  samples exist, then adjusted by slewing plus feed-forward skew
  correction, never by a step. A step after lock would misfire every
  event already queued against the old offset. An escape hatch exists
  for a large offset error (a laptop sleeping, a host restarting): it
  forces a re-lock, and the re-lock **flushes pending events with
  note-offs for everything currently held** - a stuck note is worse
  than a dropped one.
- **Known-unfixable bias.** The offset estimate is biased by half the
  network path's asymmetry, and no algorithm can remove that bias -
  the information simply is not present in round-trip measurements.
  It is small on a LAN and can be several milliseconds over an
  asymmetric path. The mitigation is a per-peer manual playout-offset
  slider (`ClockSync::SetManualOffsetUs`, clamped to +-50ms), not a
  smarter estimator - built, but not yet wired to any UI, since there
  is no GUI touching this code path until 0.5b. This is written down
  here specifically so nobody spends time trying to algorithmically fix
  something the measurements cannot resolve.
- **Poll cadence is a 1:1 multiplier on clock error, and minimum-RTT
  selection cannot filter it.** A ping waits in the responder's socket
  until that peer's next `PeerRuntime::Service`, and so does the pong
  on the way back; the two waits enter the estimate as `(d1 - d2) / 2`.

  The part that matters is that **those waits do not resample**. Both
  peers poll on a fixed period, so the phase between their loops is
  near-constant and drifts only as slowly as their periods differ -
  which makes `d1` and `d2` near-constant, and the error a systematic
  bias rather than noise. Min-RTT selection is powerless against it:
  every sample carries the same bias *and* the same RTT, so there is no
  less-contaminated sample to pick. This is the one place the
  "minimum-RTT, not averaging" rule above does not buy what it usually
  buys.

  Measured on loopback, true offset exactly zero, so every microsecond
  reported is error. The p99 tracks the poll interval at roughly 1:1:

  | poll interval | offset p99 |
  |---|---|
  | 50us | 59us |
  | 250us | 205us |
  | 1000us | 934us |
  | 4000us | 2772us |

  Were the waits independently resampled per exchange, the same code
  filters down to ~0.15x the interval instead - confirmed off-engine by
  feeding `ClockSync` both distributions.

  The practical consequence is a requirement, not an observation:
  **whatever drives `PeerRuntime::Service` should poll at or under
  ~250us.** A 1ms loop spends roughly twice criterion #1's whole 500us
  budget before the network contributes anything. `PeerRuntime` itself
  still has no knob for this - the cadence belongs entirely to the
  caller's loop - but the caller the app uses,
  `jamn_engine`'s `NetThread`, defaults to 250us and sleeps on an
  absolute schedule rather than a relative one, so the overshoot does
  not compound. It reports the interval it actually achieved
  (`NetThread::TakeCadence`, printed by `jamn_app --headless`), because
  the budget above is spent against the achieved figure, not the
  requested one. Anything else driving `Service` directly - a bench
  backend, a test harness - still picks its own cadence and is still
  bound by this paragraph. Reproduce any row above with `jamn_bench
  --backend loopback-clock --duration-seconds N --poll-interval-us N`.

  A known mitigation, not built: jittering the ping *send* time within
  the poll period would decorrelate `d1` across exchanges and hand
  min-RTT selection back its usual advantage. `ClockSync::ShouldPing`
  is where that would go.

## Clock 2: device sample clock against steady clock

Built: `modules/jamn_engine/include/jamn_engine/audio_clock.h`
(`AudioClock`). A second-order DLL (Adriaensen) at a 0.1Hz bandwidth,
chosen over a least-squares fit for one property above all: it cannot
step. Its output moves continuously by construction, so the mapping a
scheduler places events against never jumps - which is where a stuck or
double-triggered note would come from. `Prepare` is called again on
every device restart, because a restart is a new sample timeline and
carrying the loop across that gap is exactly the step it promises not to
take.

Skipping this step is what puts remote events a few milliseconds off
even after Clock 1 is correct: the audio device's own crystal is not
the CPU's crystal, and callback entry times are jittery relative to
wall-clock time. Reconciling the two - feeding `(cumulative samples,
steady-clock time at callback entry)` into a smoothing model - yields
the local device's *actual* rate (which can measurably differ from its
nominal rate) alongside a stable mapping between session time and
sample position.

The full path for turning a remote event's timestamp into "which
sample of this block does it land on" runs **entirely on the audio
thread, at block start**: session time converts to local time via
Clock 1's offset, then to a sample position via Clock 2's rate model.
The network thread never performs a timestamp conversion - there is
exactly one owner of timing authority.

## One scheduler, two resolvers

Built: `modules/jamn_engine/include/jamn_engine/event_scheduler.h`
(`EventScheduler`) and `deadline_resolver.h` (`IDeadlineResolver`,
`LiveResolver`, `MusicalResolver`). Every playable event carries a
`SessionTime` always, and a `MusicalTime` when a flag marks it as
present. There is one fixed-capacity scheduler, keyed on deadline;
only the resolver that computes "deadline" changes between live and
bar-synced modes - `IDeadlineResolver` is a runtime virtual
specifically so a live session can swap resolvers at a bar boundary
later (Phase 2b), which a compile-time template parameter couldn't do.

`LiveResolver` is live today. `MusicalResolver` exists as a type but
reports itself unimplemented (`implemented() == false`); `EventScheduler::SetResolver`
refuses to install any resolver that does, so the actual
`MusicalResolver::ComputeDeadline` body is provably unreachable, not
merely untested - it has no `TempoMap` to resolve against yet
(Phase 2b). This matters because the resolver runs on the audio thread
once one exists (see Clock 2 below); "unimplemented" can never mean
`throw` there.

**Non-negotiable, in both modes:** a player's own local input is
always monitored live, never delayed to align with anyone else's
timing. Delaying a player's own monitoring makes the app unplayable
regardless of how correct the synchronization math is elsewhere.

See `docs/PROTOCOL.md` for `SessionTime` as it appears on the wire,
and `docs/RT_RULES.md` for the real-time constraints this scheduler
and both clocks must run under.
