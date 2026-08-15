#include <catch2/catch_test_macros.hpp>
#include <array>
#include <numeric>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/instrument_assign.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::DecodeInstrumentAssign;
using jamn::proto::EncodeInstrumentAssign;
using jamn::proto::InstrumentAssign;

TEST_CASE("InstrumentAssign round-trips its reserved soundfont triple", "[proto][instrument_assign][fast]") {
    InstrumentAssign in;
    std::iota(in.bankName.begin(), in.bankName.end(), 0);
    std::iota(in.sha256.begin(), in.sha256.end(), 0);
    in.preset = 7;

    std::array<std::uint8_t, InstrumentAssign::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeInstrumentAssign(in, w));
    REQUIRE(w.Position() == InstrumentAssign::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    InstrumentAssign out;
    REQUIRE(DecodeInstrumentAssign(r, out));
    REQUIRE(out.bankName == in.bankName);
    REQUIRE(out.sha256 == in.sha256);
    REQUIRE(out.preset == 7);
}

TEST_CASE("InstrumentAssign decode fails cleanly on a truncated buffer", "[proto][instrument_assign][fast]") {
    std::array<std::uint8_t, 10> buf{};
    ByteReader r(buf.data(), buf.size());
    InstrumentAssign out;
    REQUIRE_FALSE(DecodeInstrumentAssign(r, out));
}
