#include <catch2/catch_test_macros.hpp>
#include <array>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/clock_pingpong.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using namespace jamn::proto;

TEST_CASE("ClockPing round-trips its sequence number and t1", "[proto][clock_pingpong][fast]") {
    ClockPing in;
    in.pingSeq = 0xBEEF;
    in.t1 = -1234567890123LL;  // Negative: t1 is a signed local clock reading, not a duration.

    std::array<std::uint8_t, ClockPing::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeClockPing(in, w));
    REQUIRE(w.Position() == ClockPing::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    ClockPing out;
    REQUIRE(DecodeClockPing(r, out));
    REQUIRE(out.pingSeq == in.pingSeq);
    REQUIRE(out.t1 == in.t1);
}

TEST_CASE("ClockPong round-trips all four fields", "[proto][clock_pingpong][fast]") {
    ClockPong in;
    in.pingSeq = 7;
    in.t1 = 1000;
    in.t2 = 2000;
    in.t3 = 2500;

    std::array<std::uint8_t, ClockPong::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeClockPong(in, w));
    REQUIRE(w.Position() == ClockPong::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    ClockPong out;
    REQUIRE(DecodeClockPong(r, out));
    REQUIRE(out.pingSeq == in.pingSeq);
    REQUIRE(out.t1 == in.t1);
    REQUIRE(out.t2 == in.t2);
    REQUIRE(out.t3 == in.t3);
}

TEST_CASE("ClockPing decode fails cleanly on truncated input", "[proto][clock_pingpong][fast]") {
    std::array<std::uint8_t, ClockPing::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeClockPing(ClockPing{}, w));

    for (std::size_t truncatedLen = 0; truncatedLen < ClockPing::kEncodedSize; ++truncatedLen) {
        INFO("truncated to " << truncatedLen << " bytes");
        ByteReader r(buf.data(), truncatedLen);
        ClockPing out;
        REQUIRE_FALSE(DecodeClockPing(r, out));
    }
}

TEST_CASE("ClockPong decode fails cleanly on truncated input", "[proto][clock_pingpong][fast]") {
    std::array<std::uint8_t, ClockPong::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeClockPong(ClockPong{}, w));

    for (std::size_t truncatedLen = 0; truncatedLen < ClockPong::kEncodedSize; ++truncatedLen) {
        INFO("truncated to " << truncatedLen << " bytes");
        ByteReader r(buf.data(), truncatedLen);
        ClockPong out;
        REQUIRE_FALSE(DecodeClockPong(r, out));
    }
}

TEST_CASE("A ClockPong's four fields map straight onto a ClockSyncSample's t1..t4",
          "[proto][clock_pingpong][fast]") {
    // Not a ClockSync test - jamn_proto must not depend on jamn_engine.
    // This pins the field-order claim clock_pingpong.h makes: a caller
    // builds (t1, t2, t3, t4) from (pong.t1, pong.t2, pong.t3, localNow)
    // with no extra state kept between the ping and the pong.
    ClockPong pong;
    pong.t1 = 100;
    pong.t2 = 160;
    pong.t3 = 170;
    const std::int64_t localNow = 240;

    const std::int64_t rtt = (localNow - pong.t1) - (pong.t3 - pong.t2);
    const std::int64_t offset = ((pong.t2 - pong.t1) + (pong.t3 - localNow)) / 2;
    REQUIRE(rtt == 130);
    REQUIRE(offset == -5);
}
