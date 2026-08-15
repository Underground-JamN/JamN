# ENet

Reliable UDP networking library. JamN links it behind the `ITransport` seam
(`modules/jamn_net/include/jamn_net/enet_transport.h`); nothing above
`jamn_net` includes an ENet header.

- **Upstream:** https://github.com/lsalzman/enet
- **Version:** v1.3.18
- **Commit:** `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`
- **License:** MIT (`LICENSE` here, copied verbatim from that commit)

## How it enters the build

Not vendored. The root `CMakeLists.txt` fetches it with `FetchContent`,
pinned by tag *and* commit SHA so a retagged upstream cannot silently change
the build, and only inside the `if(NOT JAMN_CORE_ONLY)` branch - the
core-only preset never fetches or links it at all. This directory exists to
carry the license and this record, per `AGENTS.md`'s "If a dependency was
added: `third_party/<name>/` has both a LICENSE and a PROVENANCE.md."

No source is modified. Two build-level facts about the upstream tree are
worth recording because both are easy to rediscover the hard way:

- It exports no usable include path of its own - its `CMakeLists.txt` uses a
  directory-scoped `include_directories()` with no `target_include_directories`
  and no ALIAS target, so JamN's root `CMakeLists.txt` attaches
  `target_include_directories(enet INTERFACE ...)` itself after
  `FetchContent_MakeAvailable`.
- It is a C project, which is why the root `project()` call enables `C`
  alongside `CXX`.

## License compatibility

MIT, same as JamN's own code. No copyleft obligation and no revenue cap; the
only requirement is that the copyright notice travel with any distributed
copy, which `LICENSE` here satisfies. See `docs/LICENSES.md` for how this
sits alongside the other dependencies.
