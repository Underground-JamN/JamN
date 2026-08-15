# Wire protocol

These rules are law today, regardless of what is built. They exist so
that two peers built from different commits do not silently corrupt
each other, and they apply to whatever gets written under
`jamn_proto`/`jamn_net`, not only to the format described below.

1. Unknown message type: skip `len` bytes and continue. **Never
   disconnect on an unknown message.**
2. Bodies may grow. Readers tolerate a larger `len` than they expect
   and skip the excess. Writers never shrink, reorder or repurpose an
   existing field.
3. `proto_major` mismatch: refuse the connection with a human-readable
   reason. `proto_minor` mismatch: operate at `min(minor)`. Finer
   distinctions belong in the `Hello` capabilities bitmask.
4. All integers little-endian and fixed width. No struct `memcpy`, no
   bitfields, no reliance on `#pragma pack`. Everything goes through
   the bounds-checked reader and writer, and that is a fuzz-tested
   invariant.
5. A `proto_major` bump requires an ADR (`docs/adr/`).

Changing the wire format means updating this file and the golden
vectors in the same PR. The golden vectors are committed binary blobs
with a byte-compare test, and they are how parallel work stops
silently breaking the protocol.

## Status: built, except EnetTransport

`jamn_proto` is real: `PacketHeader`, TLV framing, the message-type
enum below, every message struct the table below marks `Built`, golden
vectors, a fuzz-replay corpus, and join authentication are all
implemented and tested
(`modules/jamn_proto/`). `jamn_net`'s `ITransport` seam is real too,
with `SimTransport` as its one implementation so far
(`modules/jamn_net/include/jamn_net/`). What's still a locked design
decision and not code that exists: `EnetTransport` itself - ENet is
fetched by `CMakeLists.txt` (pinned to v1.3.18) but no target currently
links it or includes any of its headers - and everything in the
"Transport" table below describing how ENet's channels map onto this
protocol. That's 0.5b scope, built only after the timing core above it
has been exercised against `SimTransport` (see `docs/CLOCK.md`).

## Transport

ENet, behind the `ITransport` seam above - `jamn_net` and everything
above it talks only to that interface, never to ENet directly. Once
`EnetTransport` exists (0.5b) it will give a reliable-ordered channel
and an unsequenced-unreliable channel over **one UDP socket and one
port**, per the table below - none of this channel/ENet-flag mapping
exists yet.

| Channel | ENet flags | Contents |
|---|---|---|
| 0 Control | reliable, ordered | join/leave, session config, tempo map, transport, mixer, instrument params, note-state digest |
| 1 Realtime | **unsequenced** | note bursts, clock ping/pong, peer stats |
| 2 Bulk | reliable, fragmented | track/preset sync |
| 3+ Audio | unsequenced | Opus streams (later phase) |

**Subtle and important:** ENet's plain "unreliable" is actually
unreliable-*sequenced* and silently discards stale packets. Clock and
note traffic must use `ENET_PACKET_FLAG_UNSEQUENCED` explicitly, or
this channel silently stops behaving the way rule 1 and rule 2 assume.

## Packet shape

```
PacketHeader (8 bytes, little-endian, packed)
  u16 magic 'JM'
  u8  proto_major
  u8  proto_minor
  u16 peer_id
  u16 body_len

then TLVs, repeated:
  u16 type
  u16 len
  <len bytes>
```

Rules 1 and 2 above only make sense against this shape: an unknown
`type` is always safely skippable because `len` is always present and
trusted, and a body can always grow because the reader only ever reads
`len` bytes regardless of what it expected.

## Message types

The TLV `type` field. This table and
`modules/jamn_proto/include/jamn_proto/message_type.h`'s `MessageType` enum
must always agree - `tools/check_protocol_doc.py` enforces that as a step
in both gate scripts, on the Status column as well as the name and value,
so a message that gains a struct cannot be built here and still read
`Reserved` there. "Built" means a real message struct exists under
`jamn_proto` today; "Reserved" means the value and channel are locked so a
later phase can add the struct without a `proto_major` bump (rule 5).

| Name | Value | Channel | Status |
|---|---|---|---|
| Join | 1 | 0 Control | Built (`Hello`) |
| Leave | 2 | 0 Control | Built (`Leave`) |
| SessionConfig | 3 | 0 Control | Built (reserved `SyncMode` field) |
| TempoMap | 4 | 0 Control | Reserved |
| Transport | 5 | 0 Control | Reserved |
| Mixer | 6 | 0 Control | Reserved |
| InstrumentAssign | 7 | 0 Control | Built (reserved soundfont triple) |
| NoteStateDigest | 8 | 0 Control | Reserved |
| NoteBurst | 9 | 1 Realtime | Built |
| ClockPing | 10 | 1 Realtime | Built |
| ClockPong | 11 | 1 Realtime | Built |
| PeerStats | 12 | 1 Realtime | Reserved |

## Notes are normalized, not tunnelled

Raw MIDI has running status, no timestamps and no room for musical
time. Keyboard input, the on-screen mouse piano, and MIDI (once it
exists) are all meant to feed one shared event representation instead
of three separate paths.

**Reserved now, defined later:** a musical-time flag and field on note
events, a sync-mode selector in session config, and a soundfont
identity triple on instrument assignment. Named and laid out now so a
future addition doesn't force a `proto_major` bump - built in
`jamn_proto`, but inert (nothing populates a meaningful value into any
of them yet):

- `NoteEvent` (`modules/jamn_proto/include/jamn_proto/note_event.h`) -
  16 bytes fixed (`dt_us i32`, `event_seq u16`, `slot u8`, `kind u8`,
  `a u8`, `b u8`, `c u16`, `state_rev u16`, `flags u16`), plus an
  8-byte `t_absolute_ppq i64` tail present only when `flags` bit 0 is
  set.
- `SessionConfig` (`session_config.h`) - one reserved `sync_mode u8`
  field, always 0 today.
- `InstrumentAssign` (`instrument_assign.h`) - a reserved soundfont
  identity triple: `bank_name` (64 bytes, zero-padded ASCII), `sha256`
  (32 bytes), `preset u32`.

`Hello` (`hello.h`) is not reserved - `session_token` (32 bytes) and
`build_hash` (32 bytes) are both real and load-bearing today, per join
authentication below.

## Redundancy and dedupe (the realtime channel's loss resilience)

Built: `modules/jamn_engine/include/jamn_engine/burst_assembler.h`
(`BurstAssembler`) and `dedupe_ring.h` (`DedupeRing`). K=3: every
outgoing `NoteBurst` re-includes each of the previous 3 bursts' events
alongside whatever is newly queued, so an event rides in 4 bursts
total and survives up to 2 consecutive burst losses in a row with zero
added latency. The receiver dedupes on `(peer_id, event_seq)` in a
1024-entry ring per peer, using serial-number arithmetic
(`modules/jamn_engine/include/jamn_engine/serial_compare.h`) so a
`u16 event_seq` wrap (roughly every 11 minutes at 100 events/s) never
causes a genuinely-new event to be misread as an old duplicate.

## Join authentication

Built: `modules/jamn_proto/include/jamn_proto/join_auth.h`
(`ConstantTimeEquals`) and `packet.h`
(`DecodePacketAuthenticated`). `Hello::session_token` is compared
against the host's configured passphrase in constant time, so timing
can't leak how many leading bytes matched. A datagram from an
unrecognised `peer_id` is dropped before any TLV parsing begins at
all - `DecodePacketAuthenticated` checks the peer against the caller's
"is this peer known" predicate immediately after the header decodes,
before the body is even sliced off, so the TLV decoder is provably
unreachable for an unknown peer, not merely untested for one.

## Not specified here yet

The following is a real open design question, not an omission - ask
the maintainer rather than assuming an answer:

- Stuck-note reconciliation (the periodic held-notes digest) - Phase 1
  scope.

See `docs/CLOCK.md` for `SessionTime`, the only timestamp type this
protocol carries on the wire, and `docs/ARCHITECTURE.md` for where
`jamn_net` sits relative to the rest of the system.
