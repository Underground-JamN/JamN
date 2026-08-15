#include <catch2/catch_test_macros.hpp>
#include <array>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/packet_header.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::DecodePacketHeader;
using jamn::proto::EncodePacketHeader;
using jamn::proto::kCurrentProtoMajor;
using jamn::proto::kCurrentProtoMinor;
using jamn::proto::kMagic;
using jamn::proto::PacketHeader;

TEST_CASE("PacketHeader round-trips through encode/decode", "[proto][packet_header][fast]") {
    PacketHeader in;
    in.peerId = 42;
    in.bodyLen = 100;

    std::array<std::uint8_t, PacketHeader::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodePacketHeader(in, w));
    REQUIRE(w.Position() == PacketHeader::kEncodedSize);

    ByteReader r(buf.data(), w.Position());
    PacketHeader out;
    REQUIRE(DecodePacketHeader(r, out));
    REQUIRE(out.magic == kMagic);
    REQUIRE(out.protoMajor == kCurrentProtoMajor);
    REQUIRE(out.protoMinor == kCurrentProtoMinor);
    REQUIRE(out.peerId == 42);
    REQUIRE(out.bodyLen == 100);
}

TEST_CASE("PacketHeader magic bytes are 'J','M' on the wire", "[proto][packet_header][fast]") {
    PacketHeader in;
    std::array<std::uint8_t, PacketHeader::kEncodedSize> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodePacketHeader(in, w));
    REQUIRE(buf[0] == 'J');
    REQUIRE(buf[1] == 'M');
}

TEST_CASE("PacketHeader decode fails cleanly on a truncated buffer", "[proto][packet_header][fast]") {
    std::array<std::uint8_t, 3> buf{};
    ByteReader r(buf.data(), buf.size());
    PacketHeader out;
    REQUIRE_FALSE(DecodePacketHeader(r, out));
}
