#include <catch2/catch_test_macros.hpp>
#include <cstdint>

#include "jamn_core/session_limits.h"
#include "jamn_session/roster.h"

using namespace jamn::session;
using jamn::net::PeerId;

TEST_CASE("A roster filled to capacity refuses the next join rather than overflowing",
          "[session][roster][fast]") {
    Roster roster;
    for (std::size_t i = 0; i < Roster::kMaxPeers; ++i) {
        INFO("link " << i);
        REQUIRE(roster.OnLinkUp(static_cast<PeerId>(i)));
    }
    REQUIRE(roster.OccupiedCount() == Roster::kMaxPeers);
    REQUIRE(roster.IsFull());

    // The 9th. Refused, and the roster is unchanged - not silently written
    // past the end of its array.
    REQUIRE_FALSE(roster.OnLinkUp(static_cast<PeerId>(Roster::kMaxPeers)));
    REQUIRE(roster.OccupiedCount() == Roster::kMaxPeers);
    REQUIRE(roster.Find(static_cast<PeerId>(Roster::kMaxPeers)) == nullptr);
}

TEST_CASE("A disconnect mid-handshake leaves no half-joined slot", "[session][roster][fast]") {
    Roster roster;
    REQUIRE(roster.OnLinkUp(3));
    REQUIRE(roster.Find(3) != nullptr);
    REQUIRE(roster.Find(3)->state == PeerState::kHandshaking);
    REQUIRE(roster.JoinedCount() == 0);

    roster.OnLinkDown(3);
    REQUIRE(roster.Find(3) == nullptr);
    REQUIRE(roster.OccupiedCount() == 0);
    REQUIRE(roster.JoinedCount() == 0);
    REQUIRE_FALSE(roster.IsJoined(3));

    // And the freed slot carries nothing forward: a lookup by the peerId
    // that a later join would have used finds nothing.
    REQUIRE(roster.FindByPeerId(0) == nullptr);
}

TEST_CASE("A Leave frees the slot for the next joiner once the link is down",
          "[session][roster][fast]") {
    Roster roster;
    for (std::size_t i = 0; i < Roster::kMaxPeers; ++i) {
        REQUIRE(roster.OnLinkUp(static_cast<PeerId>(i)));
        REQUIRE(roster.MarkJoined(static_cast<PeerId>(i), static_cast<std::uint16_t>(100 + i), 0));
    }
    REQUIRE(roster.JoinedCount() == Roster::kMaxPeers);
    REQUIRE_FALSE(roster.OnLinkUp(99));  // Full.

    REQUIRE(roster.MarkLeaving(4));
    REQUIRE(roster.Find(4)->state == PeerState::kLeaving);
    // Still occupied - leaving is not gone, and the slot must not be handed
    // out while the old link is still up.
    REQUIRE(roster.IsFull());
    REQUIRE_FALSE(roster.OnLinkUp(99));

    roster.OnLinkDown(4);
    REQUIRE_FALSE(roster.IsFull());
    REQUIRE(roster.OnLinkUp(99));
    REQUIRE(roster.Find(99)->state == PeerState::kHandshaking);
    REQUIRE(roster.FindByPeerId(104) == nullptr);  // The departed peer's id is gone with it.
}

TEST_CASE("MarkJoined only promotes a handshaking link, so a replayed Join cannot rewrite an identity",
          "[session][roster][fast]") {
    Roster roster;
    REQUIRE(roster.OnLinkUp(1));
    REQUIRE(roster.MarkJoined(1, 55, 0));
    REQUIRE(roster.IsJoined(1));

    REQUIRE_FALSE(roster.MarkJoined(1, 77, 0));
    REQUIRE(roster.Find(1)->peerId == 55);
    REQUIRE_FALSE(roster.MarkJoined(2, 77, 0));  // Unknown link.
}

TEST_CASE("A duplicate connect event for a known link is not an error and claims no second slot",
          "[session][roster][fast]") {
    Roster roster;
    REQUIRE(roster.OnLinkUp(1));
    REQUIRE(roster.OnLinkUp(1));
    REQUIRE(roster.OccupiedCount() == 1);
}

TEST_CASE("MarkLeaving refuses a link that never joined", "[session][roster][fast]") {
    Roster roster;
    REQUIRE(roster.OnLinkUp(1));
    REQUIRE_FALSE(roster.MarkLeaving(1));  // Still handshaking.
    REQUIRE_FALSE(roster.MarkLeaving(2));  // Unknown.
}

TEST_CASE("FindByPeerId matches only a peer that actually has a protocol identity",
          "[session][roster][fast]") {
    Roster roster;
    REQUIRE(roster.OnLinkUp(1));
    // Handshaking: peerId defaults to 0, but no lookup may find it - it has
    // no protocol identity yet.
    REQUIRE(roster.FindByPeerId(0) == nullptr);

    REQUIRE(roster.MarkJoined(1, 42, 0));
    REQUIRE(roster.FindByPeerId(42) != nullptr);
    REQUIRE(roster.FindByPeerId(42)->link == 1);
}

TEST_CASE("The roster's capacity is the one shared peer bound, not a private copy",
          "[session][roster][fast]") {
    static_assert(Roster::kMaxPeers == jamn::core::kMaxPeers,
                  "the roster must be bounded by the same kMaxPeers every other per-peer array uses");
    REQUIRE(Roster::kMaxPeers == jamn::core::kMaxPeers);
}
