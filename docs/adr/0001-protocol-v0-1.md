# 0001: Shipping protocol v0.1 counts as the proto_major bump

## Status

Accepted.

## Context

`docs/PROTOCOL.md` rule 5 states: "A `proto_major` bump requires an ADR
(`docs/adr/`)." Phase 0.5a's `jamn_proto` work
(`maintainer/PHASE_0_5_PLAN.md`'s Wave 2) is the first commit to make
`proto_major = 1` binding on the wire - before this, `jamn_proto` was a
5-line stub with no message types, no `PacketHeader`, and nothing that
could negotiate a version at all.

The rule's wording assumes a prior version already exists and is being
changed. Going from "no protocol" to "protocol v0.1" is a different kind of
event - there is no earlier binding value to compare against, so read
literally, it is arguable whether this is a "bump" at all. That reading was
flagged as an open question rather than silently resolved
(`PHASE_0_5_PLAN.md`'s "Open questions for the maintainer" section) and
answered directly by the maintainer: yes, it counts.

## Decision

Establishing `proto_major = 1` for the first time is treated as the kind of
event rule 5's ADR requirement exists for. This file is that ADR.

The reasoning: the moment `proto_major = 1` starts shipping in commits and
in golden vectors (`modules/jamn_proto/tests/golden/`, Wave 2 T2.4), it
becomes exactly as binding on every future peer as any later bump would be
- other peers built from other commits will refuse to talk to a mismatched
`proto_major` (rule 3) starting now, not starting at whatever point a
literal reading might call the "real" first bump. There is no practical
distinction between "establishing" and "bumping" from a compatibility
standpoint; the rule's intent - a short written record before a value this
expensive to reverse goes live - applies equally to both.

## Consequences

- `proto_major = 1` is locked in as of this ADR. Changing it later (a real
  Phase 2+ breaking change to the wire format) needs its own new ADR, per
  this file's own precedent - this one is never edited to describe that
  later change.
- No process change beyond this file: future `proto_major` bumps already
  required an ADR under the existing rule. This ADR only resolves the
  ambiguity about whether the *first* value counted.
- Every future contributor asking "does starting something for the first
  time count as a bump" for some other locked-in constant in this project
  now has a precedent to point at, not just this specific case.
