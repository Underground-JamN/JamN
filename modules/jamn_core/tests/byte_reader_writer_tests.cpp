#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <limits>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;

TEST_CASE("ByteWriter/ByteReader round-trip every fixed-width integer type", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 64> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(w.WriteU8(0xAB));
    REQUIRE(w.WriteU16(0x1234));
    REQUIRE(w.WriteU32(0xDEADBEEFu));
    REQUIRE(w.WriteU64(0x0102030405060708ULL));
    REQUIRE(w.WriteI8(-1));
    REQUIRE(w.WriteI16(-12345));
    REQUIRE(w.WriteI32(-2000000000));
    REQUIRE(w.WriteI64(std::numeric_limits<std::int64_t>::min()));

    ByteReader r(buf.data(), w.Position());
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;
    std::int8_t i8 = 0;
    std::int16_t i16 = 0;
    std::int32_t i32 = 0;
    std::int64_t i64 = 0;

    REQUIRE(r.ReadU8(u8));
    REQUIRE(u8 == 0xAB);
    REQUIRE(r.ReadU16(u16));
    REQUIRE(u16 == 0x1234);
    REQUIRE(r.ReadU32(u32));
    REQUIRE(u32 == 0xDEADBEEFu);
    REQUIRE(r.ReadU64(u64));
    REQUIRE(u64 == 0x0102030405060708ULL);
    REQUIRE(r.ReadI8(i8));
    REQUIRE(i8 == -1);
    REQUIRE(r.ReadI16(i16));
    REQUIRE(i16 == -12345);
    REQUIRE(r.ReadI32(i32));
    REQUIRE(i32 == -2000000000);
    REQUIRE(r.ReadI64(i64));
    REQUIRE(i64 == std::numeric_limits<std::int64_t>::min());
    REQUIRE(r.Remaining() == 0);
}

TEST_CASE("Multi-byte integers are little-endian on the wire", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 4> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(w.WriteU32(0x01020304u));
    REQUIRE(buf[0] == 0x04);
    REQUIRE(buf[1] == 0x03);
    REQUIRE(buf[2] == 0x02);
    REQUIRE(buf[3] == 0x01);
}

TEST_CASE("ByteWriter fails, cursor unchanged, when capacity is exhausted", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 3> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(w.WriteU16(0xAAAA));
    REQUIRE(w.Position() == 2);
    // Only 1 byte of capacity remains - a u16 write must fail and leave the
    // cursor exactly where it was, not partially write.
    REQUIRE_FALSE(w.WriteU16(0xBBBB));
    REQUIRE(w.Position() == 2);
    REQUIRE(w.WriteU8(0xCC));
    REQUIRE(w.Position() == 3);
}

TEST_CASE("ByteReader fails on truncated input without reading out of bounds", "[core][byte_io][fast]") {
    // Only 1 byte available where a u32 read wants 4 - must fail, not read
    // past the buffer.
    std::array<std::uint8_t, 1> buf{0x42};
    ByteReader r(buf.data(), buf.size());
    std::uint32_t v = 0;
    REQUIRE_FALSE(r.ReadU32(v));
    REQUIRE(r.Position() == 0);

    std::uint8_t u8 = 0;
    REQUIRE(r.ReadU8(u8));
    REQUIRE(u8 == 0x42);
    REQUIRE_FALSE(r.ReadU8(u8));
}

TEST_CASE("ByteReader over a zero-length buffer fails every read and skip", "[core][byte_io][fast]") {
    ByteReader r(nullptr, 0);
    std::uint8_t u8 = 0;
    REQUIRE_FALSE(r.ReadU8(u8));
    REQUIRE_FALSE(r.Skip(1));
    REQUIRE(r.Remaining() == 0);
}

TEST_CASE("ByteReader::ReadBytes/Skip with len larger than the remaining buffer fails cleanly", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 4> buf{1, 2, 3, 4};
    ByteReader r(buf.data(), buf.size());

    std::array<std::uint8_t, 16> out{};
    REQUIRE_FALSE(r.ReadBytes(out.data(), 16));
    REQUIRE(r.Position() == 0);
    REQUIRE_FALSE(r.Skip(16));
    REQUIRE(r.Position() == 0);

    // Exactly the remaining size still succeeds.
    REQUIRE(r.ReadBytes(out.data(), 4));
    REQUIRE(out[0] == 1);
    REQUIRE(out[3] == 4);
    REQUIRE(r.Remaining() == 0);
}

TEST_CASE("ByteReader::ReadSlice scopes a child reader to exactly len bytes", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 6> buf{1, 2, 3, 4, 5, 6};
    ByteReader r(buf.data(), buf.size());

    ByteReader slice(nullptr, 0);
    REQUIRE(r.ReadSlice(slice, 3));
    REQUIRE(r.Position() == 3);
    REQUIRE(slice.Size() == 3);

    std::uint8_t v = 0;
    REQUIRE(slice.ReadU8(v));
    REQUIRE(v == 1);
    REQUIRE(slice.ReadU8(v));
    REQUIRE(v == 2);
    REQUIRE(slice.ReadU8(v));
    REQUIRE(v == 3);
    // The slice is bounded to its own 3 bytes, independent of the parent
    // reader still having 3 more bytes available.
    REQUIRE_FALSE(slice.ReadU8(v));

    REQUIRE(r.ReadU8(v));
    REQUIRE(v == 4);
}

TEST_CASE("ByteReader::ReadSlice fails, cursor unchanged, when len exceeds what remains", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 2> buf{1, 2};
    ByteReader r(buf.data(), buf.size());
    ByteReader slice(nullptr, 0);
    REQUIRE_FALSE(r.ReadSlice(slice, 3));
    REQUIRE(r.Position() == 0);
}

TEST_CASE("ByteReader::Skip advances the cursor without exposing the skipped bytes", "[core][byte_io][fast]") {
    std::array<std::uint8_t, 5> buf{1, 2, 3, 4, 5};
    ByteReader r(buf.data(), buf.size());
    REQUIRE(r.Skip(3));
    std::uint8_t u8 = 0;
    REQUIRE(r.ReadU8(u8));
    REQUIRE(u8 == 4);
}
