#include <catch2/catch_test_macros.hpp>
#include <array>
#include <numeric>
#include <vector>

#include "jamn_core/byte_writer.h"
#include "jamn_proto/hello.h"
#include "jamn_proto/join_auth.h"
#include "jamn_proto/packet.h"
#include "jamn_proto/tlv.h"

using namespace jamn::core;
using namespace jamn::proto;

TEST_CASE("ConstantTimeEquals accepts identical byte strings", "[proto][join_auth][fast]") {
    std::array<std::uint8_t, 32> a{};
    std::iota(a.begin(), a.end(), 0);
    auto b = a;
    REQUIRE(ConstantTimeEquals(a.data(), b.data(), a.size()));
}

TEST_CASE("ConstantTimeEquals rejects a mismatch at any position", "[proto][join_auth][fast]") {
    std::array<std::uint8_t, 32> a{};
    std::iota(a.begin(), a.end(), 0);

    auto mismatchFirst = a;
    mismatchFirst[0] ^= 0xFF;
    REQUIRE_FALSE(ConstantTimeEquals(a.data(), mismatchFirst.data(), a.size()));

    auto mismatchLast = a;
    mismatchLast[a.size() - 1] ^= 0xFF;
    REQUIRE_FALSE(ConstantTimeEquals(a.data(), mismatchLast.data(), a.size()));

    auto mismatchMiddle = a;
    mismatchMiddle[a.size() / 2] ^= 0xFF;
    REQUIRE_FALSE(ConstantTimeEquals(a.data(), mismatchMiddle.data(), a.size()));
}

TEST_CASE("ConstantTimeEquals over zero length is vacuously true", "[proto][join_auth][fast]") {
    std::uint8_t dummy = 0;
    REQUIRE(ConstantTimeEquals(&dummy, &dummy, 0));
}

namespace {
// Builds a minimal but well-formed packet: a header plus one TLV whose
// body would decode cleanly if the visitor were ever invoked.
std::vector<std::uint8_t> BuildWellFormedPacket(std::uint16_t peerId) {
    std::vector<std::uint8_t> buf(64);
    ByteWriter w(buf.data(), buf.size());
    PacketHeader h;
    h.peerId = peerId;
    h.bodyLen = 4 + 1;
    EncodePacketHeader(h, w);
    WriteTlvHeader(w, 3, 1);  // kSessionConfig, 1-byte body.
    w.WriteU8(0);
    buf.resize(w.Position());
    return buf;
}
}  // namespace

TEST_CASE("DecodePacketAuthenticated drops a datagram from an unrecognised peer_id "
          "without the TLV decoder ever being entered",
          "[proto][join_auth][fast]") {
    const auto data = BuildWellFormedPacket(/*peerId=*/999);

    ByteReader reader(data.data(), data.size());
    PacketHeader header;
    bool visitorCalled = false;
    const bool ok = DecodePacketAuthenticated(
        reader, header,
        [](std::uint16_t) { return false; },  // No peer_id is known.
        [&](std::uint16_t, ByteReader&) { visitorCalled = true; });

    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(visitorCalled);
}

TEST_CASE("DecodePacketAuthenticated parses the body for a recognised peer_id", "[proto][join_auth][fast]") {
    const auto data = BuildWellFormedPacket(/*peerId=*/42);

    ByteReader reader(data.data(), data.size());
    PacketHeader header;
    bool visitorCalled = false;
    const bool ok = DecodePacketAuthenticated(
        reader, header, [](std::uint16_t peerId) { return peerId == 42; },
        [&](std::uint16_t, ByteReader&) { visitorCalled = true; });

    REQUIRE(ok);
    REQUIRE(visitorCalled);
}
