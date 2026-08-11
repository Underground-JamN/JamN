# Clock and timebase

## Status: designed, not built

There is no timing code in the tree today - no timebase, sample
counter, timestamp type, block scheduler or clock-sync logic anywhere
under `modules/` or `tools/`. The nearest thing that exists is
`JuceAudioDevice`'s `sampleRate_`/`blockSize_` atomics
(`modules/jamn_platform/include/jamn_platform/juce_audio_device.h`),
which publish the device's current rate and block size and nothing
else. `modules/jamn_platform/src/juce_audio_device.cpp`'s real-time
callback deliberately discards the one place a hardware timestamp is
currently available - the `juce::AudioIODeviceCallbackContext`
parameter, via `juce::ignoreUnused(..., context)`.

Everything below is a locked design decision for `jamn_core` and
`jamn_engine` to implement, not a description of what runs today.

## Three time types, never interchangeable

| Type | Unit | Scope |
|---|---|---|
| `SessionTime` | `int64` microseconds since session epoch | shared - the only "when" on the wire |
| `SampleTime` | `int64` samples at the local device rate | local only - peers run different device rates |
| `MusicalTime` | `int64` PPQ ticks at 960 PPQ, absolute and ever-increasing | shared, derived through a tempo map |

Microseconds, specifically - not samples, because peers run different
device rates (44.1/48/96 kHz); not floats, for precision loss and
nondeterminism; not milliseconds, too coarse for a low-single-digit-ms
budget.

## Clock 1: peer offset

NTP-style exchange over the same UDP socket used for everything else:

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
  slider, not a smarter estimator. This is written down here
  specifically so nobody spends time trying to algorithmically fix
  something the measurements cannot resolve.

## Clock 2: device sample clock against steady clock

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

Every playable event carries a `SessionTime` always, and a
`MusicalTime` when a flag marks it as present. There is one
fixed-capacity scheduler, keyed on deadline; only the resolver that
computes "deadline" changes between live and bar-synced modes.

**Non-negotiable, in both modes:** a player's own local input is
always monitored live, never delayed to align with anyone else's
timing. Delaying a player's own monitoring makes the app unplayable
regardless of how correct the synchronization math is elsewhere.

See `docs/PROTOCOL.md` for `SessionTime` as it appears on the wire,
and `docs/RT_RULES.md` for the real-time constraints this scheduler
and both clocks must run under.
