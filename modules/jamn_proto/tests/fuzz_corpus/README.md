# jamn_proto fuzz corpus

Committed blobs `proto_golden_tests.cpp`'s sibling, `proto_fuzz_replay_tests.cpp`,
replays through `jamn::proto::DecodePacket` on every `ctest -L fast` run - the
corpus-replay test that satisfies docs/PROTOCOL.md rule 4's "fuzz-tested
invariant" in a project with no CI and no nightly fuzzer job.

Two kinds of entries:

- `golden_seed_*.bin` - a golden vector (`../golden/`) wrapped in a real
  packet header and TLV framing, replayed as an ordinary well-formed packet.
- `hostile_*.bin` - hand-written malformed packets, one per rule this
  protocol's bounds-checked framing has to survive: a truncated header, a
  `body_len` claiming more than the buffer actually holds, a TLV `len` of
  `0xFFFF`, a run of zero-length TLVs, and a TLV claiming to extend past its
  own body.

**The rule this corpus exists to enforce:** if a crash, an out-of-bounds
read, or a hang is ever found in this decode path - whether by hand, by a
manually-run libFuzzer target, or by anything else - the minimised
reproducer gets added here as a new committed file, permanently. It is
never just noted and discarded. The replay test's job is to make that
future failure show up on every `ctest -L fast` run, not just once.
