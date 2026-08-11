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

## Status: designed, not built

Everything below this point is a locked design decision, not a
description of code that exists. `jamn_proto` and `jamn_net` are both
5-line stub headers today - no message structs, no `ITransport`, no
encode/decode. ENet is fetched by `CMakeLists.txt` (pinned to v1.3.18)
but no target currently links it or includes any of its headers.
`tools/check_protocol_doc.py`, which is meant to assert the message
enum matches the table in this file, has nothing to check against yet.

## Transport

ENet, behind an `ITransport` seam - `jamn_net` and everything above it
talks only to that interface, never to ENet directly. It gives a
reliable-ordered channel and an unsequenced-unreliable channel over
**one UDP socket and one port**.

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

## Notes are normalized, not tunnelled

Raw MIDI has running status, no timestamps and no room for musical
time. Keyboard input, the on-screen mouse piano, and MIDI (once it
exists) are all meant to feed one shared event representation instead
of three separate paths.

**Reserved now, defined later:** a musical-time flag and field on note
events, a sync-mode selector in session config, and a soundfont
identity triple on instrument assignment. These are named here so a
future addition doesn't force a `proto_major` bump - their exact
layout is not specified yet.

## Not specified here yet

The following are real open design questions, not omissions - ask the
maintainer rather than assuming an answer:

- The redundancy/loss-resilience scheme for the realtime channel.
- Stuck-note reconciliation (the periodic held-notes digest).
- Join authentication (the session-token / `Hello` handshake).

See `docs/CLOCK.md` for `SessionTime`, the only timestamp type this
protocol carries on the wire, and `docs/ARCHITECTURE.md` for where
`jamn_net` sits relative to the rest of the system.
