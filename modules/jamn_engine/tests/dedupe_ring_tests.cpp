#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "jamn_engine/dedupe_ring.h"

using jamn::engine::DedupeRing;

TEST_CASE("DedupeRing accepts the first occurrence of a seq and flags a repeat as duplicate",
          "[engine][dedupe_ring][fast]") {
    DedupeRing ring;
    REQUIRE_FALSE(ring.IsDuplicate(1, 100));
    REQUIRE(ring.IsDuplicate(1, 100));
    REQUIRE(ring.IsDuplicate(1, 100));  // Still a duplicate on a third check.
}

TEST_CASE("DedupeRing tracks each peer independently", "[engine][dedupe_ring][fast]") {
    DedupeRing ring;
    REQUIRE_FALSE(ring.IsDuplicate(1, 100));
    // The same seq from a different peer is not a duplicate of peer 1's.
    REQUIRE_FALSE(ring.IsDuplicate(2, 100));
    REQUIRE(ring.IsDuplicate(1, 100));
    REQUIRE(ring.IsDuplicate(2, 100));
}

TEST_CASE("DedupeRing correctly discards event_seq duplicates on both sides of the u16 wrap",
          "[engine][dedupe_ring][fast]") {
    DedupeRing ring;
    // Walk seq forward across the wrap: 65530..65535, then 0..10. Every
    // one of these is genuinely new and must not be flagged a duplicate,
    // even though post-wrap values (0..10) are numerically smaller than
    // pre-wrap ones (65530..65535).
    for (std::uint32_t seq = 65530; seq <= 65535; ++seq) {
        REQUIRE_FALSE(ring.IsDuplicate(1, static_cast<std::uint16_t>(seq)));
    }
    for (std::uint16_t seq = 0; seq <= 10; ++seq) {
        REQUIRE_FALSE(ring.IsDuplicate(1, seq));
    }

    // Re-sending an already-seen value from either side of the wrap is
    // still correctly caught as a duplicate.
    REQUIRE(ring.IsDuplicate(1, 65533));
    REQUIRE(ring.IsDuplicate(1, 5));

    // And a genuinely new value continuing past what's been seen so far
    // is still accepted.
    REQUIRE_FALSE(ring.IsDuplicate(1, 11));
}

TEST_CASE("DedupeRing rejects a duplicate far older than the ring can trust, via the serial watermark",
          "[engine][dedupe_ring][fast]") {
    DedupeRing ring;
    REQUIRE_FALSE(ring.IsDuplicate(1, 100));

    // Advance the watermark well past a full ring's distance beyond 100 -
    // more than DedupeRing::kRingSize newer seq values, which is enough
    // for slot 100 % kRingSize to alias onto something else regardless of
    // whether this loop happens to revisit it.
    for (std::uint32_t seq = 101; seq <= 101 + DedupeRing::kRingSize + 10; ++seq) {
        ring.IsDuplicate(1, static_cast<std::uint16_t>(seq));
    }

    // The old seq=100 resurfacing now (a stale replay) must be rejected -
    // it's far too old to trust, watermark-wise, regardless of what its
    // ring slot currently holds.
    REQUIRE(ring.IsDuplicate(1, 100));
}
