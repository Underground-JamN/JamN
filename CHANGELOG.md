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

## [0.3] - 2026-08-16

### Added

- A player now hears their own notes. Local input is scheduled at zero
  added delay (`docs/CLOCK.md`, non-negotiable) and rendered through its
  own instrument outside `PeerMixer`, so no peer's mute, solo or volume
  can silence a player to themselves. The window's button plays through
  that path, and works with no session at all.
- An on-screen piano in the window: click a key to play it, drag across
  the keys to glide. Two octaves from C3, and its notes go to peers and
  to local monitoring by the same route a typed note will.
- A Panic button, also bound to Escape: discards everything queued and
  silences every instrument, local and per-peer, at the next audio
  block. One-shot, not a mute - notes arriving after it play normally.
  The key binding matters because with one mouse you cannot hold a note
  and click the button at the same time.

### Fixed

- Notes could sound forever if several were played inside one audio
  block - dragging fast across the on-screen piano was enough. Events
  scheduled in one block share a deadline, and the scheduler's heap was
  ordered on deadline alone, so a note-off could be delivered before its
  own note-on. Ties now break on arrival order.

## [0.2] - 2026-08-15

### Added

- Phase 0.5a's sim/clock/protocol/scheduler timing core lands headless -
  no user-visible behavior change yet, since nothing consumes it through
  the GUI until 0.5b. `jamn_proto` (`proto_major = 1`, `proto_minor = 0`
  established for the first time - `docs/adr/0001-protocol-v0-1.md`):
  `PacketHeader`, TLV framing, the message-type table, `NoteEvent`/
  `NoteBurst`/`Hello`/`SessionConfig`/`InstrumentAssign`, golden vectors,
  a fuzz-replay corpus, join authentication. `jamn_net`: `ITransport` and
  `SimTransport`, a deterministic in-process network simulator (delay,
  jitter, loss, duplication, seeded). `jamn_engine`: `ClockSync`,
  `JitterBuffer`, `EventScheduler` (with a runtime-swappable live/
  bar-synced resolver seam), K=3 burst redundancy, and receiver-side
  dedupe. A six-node, 10-virtual-minute sim acceptance suite and a
  virtual-time 30-minute clock-accuracy check (`docs/CLOCK.md`,
  `docs/PROTOCOL.md`) back all of it; `jamn_bench` gained an
  `event-scheduler` throughput backend.
- The timing core reaches a real socket. `jamn_net` gains `EnetTransport`,
  one UDP socket and one port with `docs/PROTOCOL.md`'s channel table mapped
  onto ENet's packet flags, behind a widened `ITransport` (`Poll`, peer
  connect/disconnect events, `Disconnect`). ENet is linked for the first
  time, so the build now has a `third_party/enet/` license record and a
  third ctest label, `net`, wired into both gate scripts - real-socket
  tests cannot carry `fast`, which must stay identical under `core-only`
  and the full build.
- `ClockPing` (10) and `ClockPong` (11) go from reserved to built in
  `jamn_proto`, with golden vectors. No `proto_major`/`proto_minor` change:
  still `1` / `0`, since rule 5 reserved these values precisely so filling
  them in needs no bump. `tools/check_protocol_doc.py` now also diffs the
  message table's Status column, so a built message cannot keep reading
  `Reserved` in the doc.
- `jamn_session` stops being a stub: a fixed-capacity `Roster` with an
  explicit per-peer state machine (handshaking -> joined -> leaving), and a
  `SessionHost` that decides who is allowed in - constant-time passphrase
  comparison, a `proto_major` mismatch refused with a human-readable reason,
  a `proto_minor` mismatch negotiated down. Tested headless; no GUI consumes
  it yet.
- `Leave` (2) goes from reserved to built, with a golden vector and a
  `{voluntary, kicked, timed out}` reason. Still `proto_major` 1 /
  `proto_minor` 0. An unrecognised reason decodes as voluntary rather than
  failing, per rule 2 - refusing to decode would strand the departed peer's
  roster slot.
- `jamn_dsp` gains an `IInstrument` seam and `TestToneInstrument`, a
  polyphonic sine instrument with a fixed voice pool and defined voice
  stealing. `BlipVoice` is monophonic with no pitch and no note-off, so
  until now a remote peer's note had nothing that could turn it into sound.
- Per-peer volume, mute and solo. `jamn_dsp` gains `Strip` and `PeerMixer`,
  a fixed array of one strip per peer summed into the master gain - see
  `docs/ARCHITECTURE.md` for where it sits in the signal path. Nothing
  assigns peers to strips yet.
- A master limiter. `MasterBus` now holds the summed mix to 0dBFS with a
  peak envelope behind a hard ceiling - no lookahead, because
  `docs/CLOCK.md` forbids delaying a player's own monitoring. See
  `docs/ARCHITECTURE.md`.
- `jamn_app --bench-json <path>` writes the run's audio-device reading as a
  `jamn_bench` row, from both `--headless` and the GUI. Real-device numbers
  were previously only ever printed as text, while `jamn_bench` - which owns
  the JSON schema - cannot open a device at all.
- The bench schema gains the fields a real device measures:
  `callback_interval_us`, `device_starts`, `frames`, `span_us`,
  `delivered_rate_hz` and `configured_buffer_depth_samples`. Existing rows
  are unchanged and carry -1 in all of them.
- The run loop gets a production owner. `jamn_engine` gains `PeerRuntime`,
  which owns the net thread - transport polling, the clock ping cadence and
  its pong replies, burst assembly and broadcast, and receive -> dedupe -
  and `NoteCrossing`, one `SpscRing` lane per peer, which is the single
  place a received note crosses to the audio thread. The six-node
  acceptance suite now drives the runtime instead of reimplementing it.
- A two-process acceptance run over real sockets: one host, one client,
  real ENet on 127.0.0.1, notes exchanged with zero stuck notes, under the
  `net` label. Its verdict lives in the peers' exit codes, since what it
  proves is two processes sharing no memory agreeing.
- The net thread actually runs in the app. `jamn_engine` gains `NetThread`,
  which drives `PeerRuntime::Service` on an absolute 250us schedule and
  reports the interval it achieved rather than the one it asked for; the
  cadence matters because clock-offset error tracks it roughly 1:1 (see
  `docs/CLOCK.md`).
- `jamn_app` gains `--listen <port>` and `--connect <host:port>`, so the
  shipping binary can host or join a session. With `--headless` it also
  takes `--net-ms <n>` and scores the session in its exit code, which is
  what a new `net`-labelled two-process case asserts on.
- `jamn_app` gains `--list-devices`, and `--headless` gains `--device` to
  pick one - needed for Phase 0's latency criterion, which wants ALSA
  `default` and `hw:X,Y` measured separately. Every block-timing reading
  now names the device it came from. The GUI picks its output device too,
  so two instances can use different cards.
- `jamn_app --headless` gains `--device-ms`, `--block-size` and
  `--sample-rate`, so a device can be held open for a soak and asked for a
  specific block size. Long holds report progress and still print their
  reading if interrupted.
- The blip button also sends a note to peers, so a remote note can be
  heard before the real input path exists.
- `jamn_bench` gains a `loopback-clock` backend and a
  `clock_offset_p99_us` field, measuring peer clock offset where the true
  answer is exactly zero. Run it with
  `--backend loopback-clock --duration-seconds N`.
- Clock 2 exists. `jamn_engine` gains `AudioClock`, a second-order DLL that
  measures the audio device's real sample rate against the steady clock
  instead of trusting the rate the driver reports (see `docs/CLOCK.md`).
  Nothing schedules through it yet.
- The audio callback now measures itself. `JuceAudioDevice::TakeBlockTiming`
  reports the cumulative sample count, the callback-entry times and their
  interval spread once the device is closed, and `jamn_app` prints it on
  both the `--headless` and GUI paths.
- The block-timing stats line now says whether Clock 2 has converged,
  since a short session's estimate looks equally plausible converged or
  not - the reading is meaningless until it does.
- A failed device open now says whether it was a name miss or a real open
  failure, instead of one undifferentiated error.
- A remote peer's notes reach the speakers. `jamn_engine` gains
  `AudioRuntime`, the audio thread's counterpart to `PeerRuntime`: it
  drains the note crossing at block start, converts each note out of the
  sender's timebase, schedules it, and hands back what is due. `jamn_app`
  gives each peer strip a `TestToneInstrument` and plays them. Placement is
  quantised to one audio block for now.
- `EventScheduler::FlushPeer` discards a peer's queued notes and emits an
  all-notes-off in their place, which is what a clock re-lock now costs -
  see `docs/CLOCK.md` on why a stuck note is worse than a dropped one.

### Fixed

- The first remote note of a session is no longer dropped. The playout
  target now has a floor of one audio block period, because the audio
  thread checks deadlines once per block and a 3ms target under an 11.6ms
  block made the first note late on arrival through no fault of the
  network. Found on real hardware; it cost exactly one note per session.
- Closing the window could use a destroyed `PeerRuntime`. The audio
  callback began borrowing it when remote notes started being played, but
  shutdown still destroyed it before closing the device.
- `jamn_app` no longer refuses to start a second time. Running a host and
  a client on one box is how a session is tested without a second machine,
  and JUCE's single-instance default made the second process quit before
  opening anything.
- A note's timestamp no longer shifts later when a burst is lost. Each of
  an event's four K=3 copies rides a burst with a different
  `base_t_session_us`, and `dt_us` was copied verbatim rather than
  re-derived - so whichever copy arrived first decided the instant, and a
  lost original moved it by one burst period. No wire-format change:
  `dt_us` simply carries a correct value now where it was always zero
  before. Still `proto_major` 1 / `proto_minor` 0.
- Device selection now asks for an output device only, so a card's capture
  side can no longer fail the open.

### Changed

- Gain ramps now reach their target. `MasterBus` and `Strip` share one
  time constant and one snap rule (`gain_ramp.h`); previously each kept its
  own copy, and both stalled about 2.1e-5 short when ramping up, because
  the per-sample increment fell below half a float ULP and the old
  absolute-epsilon snap could never fire. Inaudible at -93dB, but it made
  an exact target unreachable.
- The per-peer mix component is named `PeerMixer` and lives in `jamn_dsp`
  beside `Strip`/`MasterBus`. `docs/MODULE_OWNERSHIP.md` previously called
  it `MixerGraph` and put it in `jamn_engine`, which contradicted both the
  module split and the standing rule that the signal path must never become
  a graph.
- `SimTransport` honours the channel contract it previously ignored:
  `kControl` and `kBulk` never lose, reorder or duplicate, while `kRealtime`
  keeps the existing loss/jitter/duplication model. Without this, a control
  message over a lossy simulated link behaved differently than it does over
  a real one.
- The block-timing line now reports the driver's real output latency
  (`period_size * (periods - 1)` at the device's own rate/block size)
  rather than calling it a measured latency - it's a configured buffer
  depth JUCE's ALSA backend guesses at, not a round-trip measurement.

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
