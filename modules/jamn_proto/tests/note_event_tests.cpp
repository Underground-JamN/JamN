#include <catch2/catch_test_macros.hpp>
#include <array>
#include <random>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/note_event.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::DecodeNoteEvent;
using jamn::proto::EncodeNoteEvent;
using jamn::proto::kMusicalTimeFlag;
using jamn::proto::NoteEvent;
using jamn::proto::NoteEventKind;

TEST_CASE("NoteEvent without the musical-time flag encodes to exactly 16 bytes", "[proto][note_event][fast]") {
    NoteEvent e;
    e.flags = 0;  // kMusicalTimeFlag not set.

    std::array<std::uint8_t, 64> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeNoteEvent(e, w));
    REQUIRE(w.Position() == NoteEvent::kFixedEncodedSize);
    REQUIRE(w.Position() == 16);
}

TEST_CASE("NoteEvent with bit0 set round-trips its t_absolute_ppq tail", "[proto][note_event][fast]") {
    NoteEvent e;
    e.flags = kMusicalTimeFlag;
    e.tAbsolutePpq = 123456789;

    std::array<std::uint8_t, 64> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeNoteEvent(e, w));
    REQUIRE(w.Position() == NoteEvent::kFixedEncodedSize + NoteEvent::kTailEncodedSize);

    ByteReader r(buf.data(), w.Position());
    NoteEvent out;
    REQUIRE(DecodeNoteEvent(r, out));
    REQUIRE(out.tAbsolutePpq == 123456789);
    REQUIRE(out == e);
}

TEST_CASE("NoteEvent round-trips over randomised field values", "[proto][note_event][fast]") {
    // Fixed seed: deterministic, reproducible failures rather than a flaky
    // sweep - this project has no CI to re-run a flake on.
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> byteDist(0, 255);
    std::uniform_int_distribution<int> u16Dist(0, 65535);
    std::uniform_int_distribution<long> i32Dist(-2000000000, 2000000000);
    std::uniform_int_distribution<long long> i64Dist(-1'000'000'000'000LL, 1'000'000'000'000LL);
    std::uniform_int_distribution<int> boolDist(0, 1);
    std::uniform_int_distribution<int> kindDist(0, 2);

    for (int i = 0; i < 500; ++i) {
        NoteEvent e;
        e.dtUs = static_cast<std::int32_t>(i32Dist(rng));
        e.eventSeq = static_cast<std::uint16_t>(u16Dist(rng));
        e.slot = static_cast<std::uint8_t>(byteDist(rng));
        e.kind = static_cast<NoteEventKind>(kindDist(rng));
        e.a = static_cast<std::uint8_t>(byteDist(rng));
        e.b = static_cast<std::uint8_t>(byteDist(rng));
        e.c = static_cast<std::uint16_t>(u16Dist(rng));
        e.stateRev = static_cast<std::uint16_t>(u16Dist(rng));
        e.flags = boolDist(rng) ? kMusicalTimeFlag : 0;
        e.tAbsolutePpq = static_cast<std::int64_t>(i64Dist(rng));

        std::array<std::uint8_t, 64> buf{};
        ByteWriter w(buf.data(), buf.size());
        REQUIRE(EncodeNoteEvent(e, w));

        ByteReader r(buf.data(), w.Position());
        NoteEvent out;
        REQUIRE(DecodeNoteEvent(r, out));
        REQUIRE(out == e);
        REQUIRE(r.Remaining() == 0);
    }
}

TEST_CASE("NoteEvent decode fails cleanly on a truncated buffer", "[proto][note_event][fast]") {
    std::array<std::uint8_t, 5> buf{};
    ByteReader r(buf.data(), buf.size());
    NoteEvent out;
    REQUIRE_FALSE(DecodeNoteEvent(r, out));
}
