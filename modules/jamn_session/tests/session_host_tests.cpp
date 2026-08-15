#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_net/sim_transport.h"
#include "jamn_proto/hello.h"
#include "jamn_proto/leave.h"
#include "jamn_proto/message_type.h"
#include "jamn_proto/packet_header.h"
#include "jamn_proto/tlv.h"
#include "jamn_session/session_host.h"

using namespace jamn::net;
using namespace jamn::proto;
using namespace jamn::session;
using jamn::core::ByteReader;
using jamn::core::ByteWriter;

namespace {

std::array<std::uint8_t, Hello::kSessionTokenBytes> MakeToken(std::uint8_t seed) {
    std::array<std::uint8_t, Hello::kSessionTokenBytes> token{};
    std::iota(token.begin(), token.end(), seed);
    return token;
}

// One packet: header plus a single TLV. protoMajor/protoMinor are
// parameters because the refusal path is precisely about a header a
// mismatched peer would send.
std::vector<std::uint8_t> BuildPacket(MessageType type, const std::vector<std::uint8_t>& value,
                                       std::uint16_t peerId = 0,
                                       std::uint8_t protoMajor = kCurrentProtoMajor,
                                       std::uint8_t protoMinor = kCurrentProtoMinor) {
    std::vector<std::uint8_t> buf(512);
    ByteWriter w(buf.data(), buf.size());

    PacketHeader header;
    header.protoMajor = protoMajor;
    header.protoMinor = protoMinor;
    header.peerId = peerId;
    header.bodyLen = static_cast<std::uint16_t>(4 + value.size());
    REQUIRE(EncodePacketHeader(header, w));
    REQUIRE(WriteTlvHeader(w, static_cast<std::uint16_t>(type), static_cast<std::uint16_t>(value.size())));
    REQUIRE(w.WriteBytes(value.data(), value.size()));

    buf.resize(w.Position());
    return buf;
}

std::vector<std::uint8_t> EncodeHelloValue(const std::array<std::uint8_t, Hello::kSessionTokenBytes>& token) {
    Hello hello;
    hello.sessionToken = token;
    std::vector<std::uint8_t> value(Hello::kEncodedSize);
    ByteWriter w(value.data(), value.size());
    REQUIRE(EncodeHello(hello, w));
    value.resize(w.Position());
    return value;
}

// A host bound to a SimTransport, with the correct passphrase configured.
// kClientLink exists as a real node in the sim, not just as a number, so a
// Send to it fails only when the link was genuinely torn down - a
// nonexistent node would fail for "no route" and prove nothing.
constexpr PeerId kClientLink = 1;

struct HostFixture {
    SimNetwork net{/*seed=*/1};
    SimTransport& hostTransport = net.CreateNode(0);
    SimTransport& clientTransport = net.CreateNode(kClientLink);
    SessionHost host{hostTransport};
    std::array<std::uint8_t, Hello::kSessionTokenBytes> token = MakeToken(7);
    std::vector<std::pair<PeerId, std::string>> refusals;

    HostFixture() {
        host.SetSessionToken(token);
        host.SetRefusalCallback(
            [this](PeerId link, const std::string& reason) { refusals.emplace_back(link, reason); });
    }

    bool Feed(PeerId link, const std::vector<std::uint8_t>& packet) {
        ByteReader reader(packet.data(), packet.size());
        return host.HandleControlPacket(link, reader);
    }
};

}  // namespace

TEST_CASE("A correct Join is accepted and assigned a nonzero protocol peer_id",
          "[session][host_authority][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);
    REQUIRE(fixture.Feed(1, BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token))));

    REQUIRE(fixture.host.roster().IsJoined(1));
    const RosterEntry* entry = fixture.host.roster().Find(1);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->peerId != 0);
    REQUIRE(fixture.refusals.empty());
}

TEST_CASE("A mismatched proto_major yields a non-empty reason string and no roster slot",
          "[session][host_authority][refuse][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);

    const auto packet = BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token), /*peerId=*/0,
                                     static_cast<std::uint8_t>(kCurrentProtoMajor + 1));
    REQUIRE(fixture.Feed(1, packet));

    REQUIRE(fixture.refusals.size() == 1);
    REQUIRE(fixture.refusals[0].first == 1);
    REQUIRE_FALSE(fixture.refusals[0].second.empty());

    REQUIRE_FALSE(fixture.host.roster().IsJoined(1));
    REQUIRE(fixture.host.roster().Find(1) == nullptr);
    REQUIRE(fixture.host.roster().OccupiedCount() == 0);

    // The link really was dropped, not just forgotten - the contrast case
    // for the unknown-message-type test, where the same Send must succeed.
    const std::uint8_t payload[] = {1};
    REQUIRE_FALSE(fixture.hostTransport.Send(kClientLink, Channel::kControl, payload, sizeof(payload)));
}

TEST_CASE("A wrong session passphrase is refused with a reason and no roster slot",
          "[session][host_authority][refuse][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);
    REQUIRE(fixture.Feed(1, BuildPacket(MessageType::kJoin, EncodeHelloValue(MakeToken(99)))));

    REQUIRE(fixture.refusals.size() == 1);
    REQUIRE_FALSE(fixture.refusals[0].second.empty());
    REQUIRE(fixture.host.roster().Find(1) == nullptr);
}

TEST_CASE("A proto_minor mismatch is negotiated down rather than refused",
          "[session][host_authority][fast]") {
    // Rule 3: minor mismatch operates at min(minor), only major refuses.
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);
    const auto packet = BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token), /*peerId=*/0,
                                     kCurrentProtoMajor, static_cast<std::uint8_t>(kCurrentProtoMinor + 5));
    REQUIRE(fixture.Feed(1, packet));

    REQUIRE(fixture.refusals.empty());
    REQUIRE(fixture.host.roster().IsJoined(1));
}

TEST_CASE("An unknown message type on a joined peer does not disconnect it",
          "[session][host_authority][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);
    REQUIRE(fixture.Feed(1, BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token))));
    const std::uint16_t assigned = fixture.host.roster().Find(1)->peerId;

    // A type nothing in this build knows, with a body of its own. Protocol
    // rule 1: skip len bytes and continue - never disconnect.
    const auto unknown = BuildPacket(static_cast<MessageType>(0xBEEF), {1, 2, 3, 4, 5}, assigned);
    REQUIRE(fixture.Feed(1, unknown));

    REQUIRE(fixture.host.roster().IsJoined(1));
    REQUIRE(fixture.refusals.empty());
    // And the link is genuinely still usable, not merely still listed.
    const std::uint8_t payload[] = {1};
    REQUIRE(fixture.hostTransport.Send(1, Channel::kControl, payload, sizeof(payload)));
}

TEST_CASE("A Leave from a joined peer moves it to leaving and frees the slot when the link drops",
          "[session][host_authority][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);
    REQUIRE(fixture.Feed(1, BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token))));
    const std::uint16_t assigned = fixture.host.roster().Find(1)->peerId;

    Leave leave;
    leave.peerId = assigned;
    leave.reason = LeaveReason::kVoluntary;
    std::vector<std::uint8_t> value(Leave::kEncodedSize);
    ByteWriter w(value.data(), value.size());
    REQUIRE(EncodeLeave(leave, w));
    value.resize(w.Position());

    REQUIRE(fixture.Feed(1, BuildPacket(MessageType::kLeave, value, assigned)));
    REQUIRE(fixture.host.roster().Find(1)->state == PeerState::kLeaving);

    fixture.host.HandlePeerEvent(1, PeerEvent::kDisconnected);
    REQUIRE(fixture.host.roster().Find(1) == nullptr);
}

TEST_CASE("A joined link speaking as someone else's peer_id is dropped, not disconnected",
          "[session][host_authority][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);
    REQUIRE(fixture.Feed(1, BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token))));
    const std::uint16_t assigned = fixture.host.roster().Find(1)->peerId;

    const auto spoofed = BuildPacket(MessageType::kLeave, {0, 0, 0},
                                      static_cast<std::uint16_t>(assigned + 1));
    REQUIRE_FALSE(fixture.Feed(1, spoofed));  // Not acted on...
    REQUIRE(fixture.host.roster().IsJoined(1));  // ...and the peer is still here.
    REQUIRE(fixture.refusals.empty());
}

TEST_CASE("A connect beyond capacity is refused at the peer event, before any Join is read",
          "[session][host_authority][refuse][fast]") {
    HostFixture fixture;
    for (std::size_t i = 0; i < Roster::kMaxPeers; ++i) {
        fixture.host.HandlePeerEvent(static_cast<PeerId>(i + 1), PeerEvent::kConnected);
    }
    REQUIRE(fixture.host.roster().OccupiedCount() == Roster::kMaxPeers);

    fixture.host.HandlePeerEvent(99, PeerEvent::kConnected);
    REQUIRE(fixture.refusals.size() == 1);
    REQUIRE(fixture.refusals[0].first == 99);
    REQUIRE_FALSE(fixture.refusals[0].second.empty());
    REQUIRE(fixture.host.roster().OccupiedCount() == Roster::kMaxPeers);
}

TEST_CASE("A control packet from a link with no roster entry is dropped",
          "[session][host_authority][fast]") {
    HostFixture fixture;
    REQUIRE_FALSE(fixture.Feed(7, BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token))));
    REQUIRE(fixture.host.roster().Find(7) == nullptr);
    REQUIRE(fixture.refusals.empty());
}

TEST_CASE("A truncated or wrong-magic packet is dropped without refusing the peer",
          "[session][host_authority][fast]") {
    HostFixture fixture;
    fixture.host.HandlePeerEvent(1, PeerEvent::kConnected);

    const std::vector<std::uint8_t> truncated = {0x4A};
    REQUIRE_FALSE(fixture.Feed(1, truncated));

    auto wrongMagic = BuildPacket(MessageType::kJoin, EncodeHelloValue(fixture.token));
    wrongMagic[0] = 0x00;
    REQUIRE_FALSE(fixture.Feed(1, wrongMagic));

    REQUIRE(fixture.refusals.empty());
    REQUIRE(fixture.host.roster().Find(1) != nullptr);  // Still handshaking, not torn down.
}

TEST_CASE("EvaluateJoin decides without touching the roster", "[session][host_authority][fast]") {
    HostFixture fixture;

    PacketHeader good;
    Hello hello;
    hello.sessionToken = fixture.token;
    const JoinOutcome accepted = fixture.host.EvaluateJoin(good, hello);
    REQUIRE(accepted.accepted);
    REQUIRE(accepted.reason.empty());

    PacketHeader badMajor;
    badMajor.protoMajor = static_cast<std::uint8_t>(kCurrentProtoMajor + 1);
    const JoinOutcome refused = fixture.host.EvaluateJoin(badMajor, hello);
    REQUIRE_FALSE(refused.accepted);
    REQUIRE_FALSE(refused.reason.empty());

    REQUIRE(fixture.host.roster().OccupiedCount() == 0);
}
