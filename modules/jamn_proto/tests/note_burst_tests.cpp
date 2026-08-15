#include <catch2/catch_test_macros.hpp>
#include <array>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/note_burst.h"

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::proto::DecodeNoteBurst;
using jamn::proto::EncodeNoteBurst;
using jamn::proto::kMaxEventsPerBurst;
using jamn::proto::NoteBurst;
using jamn::proto::NoteEvent;
using jamn::proto::NoteEventKind;

namespace {
NoteEvent MakeEvent(std::uint16_t seq) {
    NoteEvent e;
    e.eventSeq = seq;
    e.kind = NoteEventKind::kNoteOn;
    e.a = 60;
    e.b = 100;
    return e;
}
}  // namespace

TEST_CASE("NoteBurst with zero events round-trips its header only", "[proto][note_burst][fast]") {
    NoteBurst burst;
    burst.baseTSessionUs = 42;
    burst.burstSeq = 7;
    burst.eventCount = 0;

    std::array<std::uint8_t, 256> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeNoteBurst(burst, w));
    REQUIRE(w.Position() == NoteBurst::kHeaderEncodedSize);

    ByteReader r(buf.data(), w.Position());
    NoteBurst out;
    REQUIRE(DecodeNoteBurst(r, out));
    REQUIRE(out.baseTSessionUs == 42);
    REQUIRE(out.burstSeq == 7);
    REQUIRE(out.eventCount == 0);
}

TEST_CASE("NoteBurst round-trips several events in order", "[proto][note_burst][fast]") {
    NoteBurst burst;
    burst.baseTSessionUs = 1000;
    burst.burstSeq = 1;
    burst.eventCount = 3;
    burst.events[0] = MakeEvent(1);
    burst.events[1] = MakeEvent(2);
    burst.events[2] = MakeEvent(3);

    std::array<std::uint8_t, 256> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(EncodeNoteBurst(burst, w));

    ByteReader r(buf.data(), w.Position());
    NoteBurst out;
    REQUIRE(DecodeNoteBurst(r, out));
    REQUIRE(out.eventCount == 3);
    REQUIRE(out.events[0].eventSeq == 1);
    REQUIRE(out.events[1].eventSeq == 2);
    REQUIRE(out.events[2].eventSeq == 3);
}

TEST_CASE("NoteBurst decode rejects an event_count above kMaxEventsPerBurst without touching the array out of bounds",
          "[proto][note_burst][fast]") {
    // Hand-craft a header claiming more events than the fixed-capacity
    // array can hold - a malformed/hostile packet, not just a large burst.
    std::array<std::uint8_t, 32> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(w.WriteI64(0));
    REQUIRE(w.WriteU16(0));
    REQUIRE(w.WriteU8(static_cast<std::uint8_t>(kMaxEventsPerBurst + 1)));
    REQUIRE(w.WriteU8(0));

    ByteReader r(buf.data(), w.Position());
    NoteBurst out;
    REQUIRE_FALSE(DecodeNoteBurst(r, out));
}

TEST_CASE("NoteBurst decode fails cleanly when a contained NoteEvent is truncated", "[proto][note_burst][fast]") {
    std::array<std::uint8_t, 32> buf{};
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(w.WriteI64(0));
    REQUIRE(w.WriteU16(0));
    REQUIRE(w.WriteU8(1));  // Claims 1 event.
    REQUIRE(w.WriteU8(0));
    // ...but no event bytes follow.

    ByteReader r(buf.data(), w.Position());
    NoteBurst out;
    REQUIRE_FALSE(DecodeNoteBurst(r, out));
}
