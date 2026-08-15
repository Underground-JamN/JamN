#include <catch2/catch_test_macros.hpp>
#include <array>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/leave.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using namespace jamn::proto;

TEST_CASE("Leave round-trips its peerId and reason", "[proto][leave][fast]") {
    Leave in;
    in.peerId = 0x0102;
    in.reason = LeaveReason::kKicked;

    std::array<std::uint8_t, Leave::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeLeave(in, w));
    REQUIRE(w.Position() == Leave::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    Leave out;
    REQUIRE(DecodeLeave(r, out));
    REQUIRE(out.peerId == in.peerId);
    REQUIRE(out.reason == LeaveReason::kKicked);
}

TEST_CASE("Leave decodes an unrecognised reason as kVoluntary rather than failing",
          "[proto][leave][fast]") {
    // docs/PROTOCOL.md rule 2: a reader tolerates what a newer writer sent.
    // Refusing to decode here would leave the roster holding a slot open for
    // a peer that has already gone.
    const std::uint8_t bytes[] = {0x05, 0x00, 0xFE};
    ByteReader r(bytes, sizeof(bytes));
    Leave out;
    REQUIRE(DecodeLeave(r, out));
    REQUIRE(out.peerId == 5);
    REQUIRE(out.reason == LeaveReason::kVoluntary);
}

TEST_CASE("Leave decode fails cleanly on truncated input", "[proto][leave][fast]") {
    const std::uint8_t bytes[] = {0x01, 0x00, 0x00};
    for (std::size_t truncatedLen = 0; truncatedLen < Leave::kEncodedSize; ++truncatedLen) {
        INFO("truncated to " << truncatedLen << " bytes");
        ByteReader r(bytes, truncatedLen);
        Leave out;
        REQUIRE_FALSE(DecodeLeave(r, out));
    }
}
