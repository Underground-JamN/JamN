#include <catch2/catch_test_macros.hpp>
#include <array>
#include <vector>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/tlv.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::ForEachTlv;
using jamn::proto::ReadTlvHeader;
using jamn::proto::TlvHeader;
using jamn::proto::WriteTlvHeader;

TEST_CASE("TlvHeader round-trips through encode/decode", "[proto][tlv][fast]") {
    std::array<std::uint8_t, 4> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(WriteTlvHeader(w, 7, 100));

    ByteReader r(buf.data(), buf.size());
    TlvHeader h;
    REQUIRE(ReadTlvHeader(r, h));
    REQUIRE(h.type == 7);
    REQUIRE(h.len == 100);
}

TEST_CASE("ForEachTlv visits every TLV in order with correctly scoped value readers", "[proto][tlv][fast]") {
    std::array<std::uint8_t, 64> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(WriteTlvHeader(w, 1, 2));
    REQUIRE(w.WriteU8(0xAA));
    REQUIRE(w.WriteU8(0xBB));
    REQUIRE(WriteTlvHeader(w, 2, 1));
    REQUIRE(w.WriteU8(0xCC));

    ByteReader r(buf.data(), w.Position());
    std::vector<std::uint16_t> visitedTypes;
    std::vector<std::uint8_t> firstBytes;
    const bool ok = ForEachTlv(r, [&](std::uint16_t type, ByteReader& value) {
        visitedTypes.push_back(type);
        std::uint8_t first = 0;
        REQUIRE(value.ReadU8(first));
        firstBytes.push_back(first);
    });

    REQUIRE(ok);
    REQUIRE(visitedTypes == std::vector<std::uint16_t>{1, 2});
    REQUIRE(firstBytes == std::vector<std::uint8_t>{0xAA, 0xCC});
}

TEST_CASE("ForEachTlv skips an unknown type and still decodes the following TLV", "[proto][tlv][fast]") {
    std::array<std::uint8_t, 64> buf{};
    ByteWriter w(buf.data(), buf.size());
    // An "unknown" type (9999) whose value the visitor never reads from.
    REQUIRE(WriteTlvHeader(w, 9999, 3));
    REQUIRE(w.WriteU8(1));
    REQUIRE(w.WriteU8(2));
    REQUIRE(w.WriteU8(3));
    REQUIRE(WriteTlvHeader(w, 1, 1));
    REQUIRE(w.WriteU8(0x42));

    ByteReader r(buf.data(), w.Position());
    std::vector<std::uint16_t> visitedTypes;
    std::uint8_t knownValue = 0;
    const bool ok = ForEachTlv(r, [&](std::uint16_t type, ByteReader& value) {
        visitedTypes.push_back(type);
        if (type == 9999) {
            return;  // Unknown type - the visitor deliberately reads nothing.
        }
        REQUIRE(value.ReadU8(knownValue));
    });

    REQUIRE(ok);
    REQUIRE(visitedTypes == std::vector<std::uint16_t>{9999, 1});
    REQUIRE(knownValue == 0x42);
}

TEST_CASE("ForEachTlv tolerates a body longer than the visitor expects", "[proto][tlv][fast]") {
    std::array<std::uint8_t, 64> buf{};
    ByteWriter w(buf.data(), buf.size());
    // len=5 but the visitor only reads the first byte - rule 2's "bodies
    // may grow" in miniature.
    REQUIRE(WriteTlvHeader(w, 1, 5));
    for (std::uint8_t i = 0; i < 5; ++i) REQUIRE(w.WriteU8(i));
    REQUIRE(WriteTlvHeader(w, 2, 1));
    REQUIRE(w.WriteU8(0x99));

    ByteReader r(buf.data(), w.Position());
    std::vector<std::uint16_t> visitedTypes;
    const bool ok = ForEachTlv(r, [&](std::uint16_t type, ByteReader& value) {
        visitedTypes.push_back(type);
        if (type == 1) {
            std::uint8_t first = 0;
            REQUIRE(value.ReadU8(first));
            REQUIRE(first == 0);
            // Deliberately not reading the other 4 bytes.
        }
    });

    REQUIRE(ok);
    REQUIRE(visitedTypes == std::vector<std::uint16_t>{1, 2});
}

TEST_CASE("ForEachTlv fails on a length that runs past the buffer", "[proto][tlv][fast]") {
    std::array<std::uint8_t, 4> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(WriteTlvHeader(w, 1, 100));  // claims 100 bytes follow; none do.

    ByteReader r(buf.data(), w.Position());
    const bool ok = ForEachTlv(r, [](std::uint16_t, ByteReader&) {});
    REQUIRE_FALSE(ok);
}

TEST_CASE("ForEachTlv over an empty buffer visits nothing and succeeds", "[proto][tlv][fast]") {
    ByteReader r(nullptr, 0);
    int visits = 0;
    const bool ok = ForEachTlv(r, [&](std::uint16_t, ByteReader&) { ++visits; });
    REQUIRE(ok);
    REQUIRE(visits == 0);
}
