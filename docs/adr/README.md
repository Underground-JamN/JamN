# Architecture decision records

Two rules already point here: `docs/RT_RULES.md` requires an ADR to
add a new lock-free primitive alongside `SpscRing`, and
`docs/PROTOCOL.md` requires one for a `proto_major` bump. Both are
decisions expensive enough to reverse that they need a short written
record, not just a PR description.

## Format

One file per decision: `docs/adr/NNNN-short-title.md`, numbered
sequentially starting at `0001`. Each file has four sections:

- **Status** - proposed, accepted, or superseded (with a pointer to
  the ADR that supersedes it).
- **Context** - what forced this decision; what would go wrong without
  one.
- **Decision** - what was decided, stated plainly.
- **Consequences** - what this commits the project to, including
  anything it rules out.

An ADR is never edited after acceptance to reflect a later change of
mind - a later decision that reverses or narrows one gets its own new
file, and the old one is marked superseded.
