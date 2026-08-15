#include <catch2/catch_test_macros.hpp>
#include <array>
#include <numeric>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/hello.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::DecodeHello;
using jamn::proto::EncodeHello;
using jamn::proto::Hello;

TEST_CASE("Hello round-trips through encode/decode", "[proto][hello][fast]") {
    Hello in;
    std::iota(in.sessionToken.begin(), in.sessionToken.end(), 0);
    std::iota(in.buildHash.begin(), in.buildHash.end(), 100);
    in.instrumentBankVersion = 3;
    in.capabilities = 0xF00D;

    std::array<std::uint8_t, Hello::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeHello(in, w));
    REQUIRE(w.Position() == Hello::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    Hello out;
    REQUIRE(DecodeHello(r, out));
    REQUIRE(out.sessionToken == in.sessionToken);
    REQUIRE(out.buildHash == in.buildHash);
    REQUIRE(out.instrumentBankVersion == 3);
    REQUIRE(out.capabilities == 0xF00D);
}

TEST_CASE("Hello decode fails cleanly on a truncated buffer", "[proto][hello][fast]") {
    std::array<std::uint8_t, 10> buf{};
    ByteReader r(buf.data(), buf.size());
    Hello out;
    REQUIRE_FALSE(DecodeHello(r, out));
}
