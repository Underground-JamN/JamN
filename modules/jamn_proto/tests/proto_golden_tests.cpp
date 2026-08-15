// Byte-compare tests against modules/jamn_proto/tests/golden/*.bin - the
// location docs/PROTOCOL.md and AGENTS.md require but don't place
// (PHASE_0_5_PLAN.md's T2.4 decision: the repo's own convention is
// per-module tests/, with no root tests/ directory anywhere, so this
// follows that convention). Each committed blob was produced once by
// running the real Encode* function over a fixed sample value and saving
// the output - these tests exist to catch any future change to the wire
// format, not to describe how the blobs were made.
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

#include "jamn_core/byte_writer.h"
#include "jamn_proto/clock_pingpong.h"
#include "jamn_proto/hello.h"
#include "jamn_proto/instrument_assign.h"
#include "jamn_proto/leave.h"
#include "jamn_proto/note_burst.h"
#include "jamn_proto/note_event.h"
#include "jamn_proto/packet_header.h"
#include "jamn_proto/session_config.h"

using namespace jamn::core;
using namespace jamn::proto;

namespace {

std::vector<std::uint8_t> ReadGoldenFile(const std::string& name) {
    const std::string path = std::string(JAMN_PROTO_GOLDEN_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.is_open());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

template <typename Encoder>
void RequireMatchesGolden(const std::string& name, Encoder&& encode, std::size_t maxSize) {
    std::vector<std::uint8_t> buf(maxSize);
    ByteWriter w(buf.data(), buf.size());
    REQUIRE(encode(w));
    buf.resize(w.Position());

    const std::vector<std::uint8_t> golden = ReadGoldenFile(name);
    INFO("golden file: " << name);
    REQUIRE(buf == golden);
}

}  // namespace

TEST_CASE("golden: PacketHeader matches its committed vector", "[proto][golden][fast]") {
    PacketHeader h;
    h.peerId = 0x1234;
    h.bodyLen = 0x0056;
    RequireMatchesGolden(
        "packet_header.bin", [&](ByteWriter& w) { return EncodePacketHeader(h, w); }, 64);
}

TEST_CASE("golden: NoteEvent without musical time matches its committed vector", "[proto][golden][fast]") {
    NoteEvent e;
    e.dtUs = -1000;
    e.eventSeq = 42;
    e.slot = 3;
    e.kind = NoteEventKind::kNoteOn;
    e.a = 60;
    e.b = 100;
    e.c = 7;
    e.stateRev = 2;
    e.flags = 0;
    RequireMatchesGolden(
        "note_event_fixed.bin", [&](ByteWriter& w) { return EncodeNoteEvent(e, w); }, 64);
}

TEST_CASE("golden: NoteEvent with musical time matches its committed vector", "[proto][golden][fast]") {
    NoteEvent e;
    e.dtUs = 500;
    e.eventSeq = 7;
    e.slot = 1;
    e.kind = NoteEventKind::kNoteOff;
    e.a = 1;
    e.b = 2;
    e.c = 3;
    e.stateRev = 4;
    e.flags = kMusicalTimeFlag;
    e.tAbsolutePpq = 1234567890123;
    RequireMatchesGolden(
        "note_event_with_tail.bin", [&](ByteWriter& w) { return EncodeNoteEvent(e, w); }, 64);
}

TEST_CASE("golden: NoteBurst matches its committed vector", "[proto][golden][fast]") {
    NoteBurst burst;
    burst.baseTSessionUs = 987654321;
    burst.burstSeq = 5;
    burst.eventCount = 2;
    burst.events[0].eventSeq = 1;
    burst.events[0].kind = NoteEventKind::kNoteOn;
    burst.events[0].a = 60;
    burst.events[0].b = 100;
    burst.events[1].eventSeq = 2;
    burst.events[1].kind = NoteEventKind::kNoteOff;
    burst.events[1].a = 60;
    burst.events[1].b = 0;
    RequireMatchesGolden(
        "note_burst.bin", [&](ByteWriter& w) { return EncodeNoteBurst(burst, w); }, 256);
}

TEST_CASE("golden: Hello matches its committed vector", "[proto][golden][fast]") {
    Hello h;
    std::iota(h.sessionToken.begin(), h.sessionToken.end(), 0);
    std::iota(h.buildHash.begin(), h.buildHash.end(), 100);
    h.instrumentBankVersion = 3;
    h.capabilities = 0xF00D;
    RequireMatchesGolden(
        "hello.bin", [&](ByteWriter& w) { return EncodeHello(h, w); }, 256);
}

TEST_CASE("golden: SessionConfig matches its committed vector", "[proto][golden][fast]") {
    SessionConfig c;
    c.syncMode = 5;
    RequireMatchesGolden(
        "session_config.bin", [&](ByteWriter& w) { return EncodeSessionConfig(c, w); }, 16);
}

TEST_CASE("golden: Leave matches its committed vector", "[proto][golden][fast]") {
    Leave l;
    l.peerId = 0x0102;
    l.reason = LeaveReason::kKicked;
    RequireMatchesGolden(
        "leave.bin", [&](ByteWriter& w) { return EncodeLeave(l, w); }, 16);
}

TEST_CASE("golden: ClockPing matches its committed vector", "[proto][golden][fast]") {
    ClockPing p;
    p.pingSeq = 0xBEEF;
    p.t1 = -1234567890123LL;
    RequireMatchesGolden(
        "clock_ping.bin", [&](ByteWriter& w) { return EncodeClockPing(p, w); }, 64);
}

TEST_CASE("golden: ClockPong matches its committed vector", "[proto][golden][fast]") {
    ClockPong p;
    p.pingSeq = 7;
    p.t1 = 1000;
    p.t2 = 2000;
    p.t3 = 2500;
    RequireMatchesGolden(
        "clock_pong.bin", [&](ByteWriter& w) { return EncodeClockPong(p, w); }, 64);
}

TEST_CASE("golden: InstrumentAssign matches its committed vector", "[proto][golden][fast]") {
    InstrumentAssign ia;
    std::iota(ia.bankName.begin(), ia.bankName.end(), 0);
    std::iota(ia.sha256.begin(), ia.sha256.end(), 0);
    ia.preset = 7;
    RequireMatchesGolden(
        "instrument_assign.bin", [&](ByteWriter& w) { return EncodeInstrumentAssign(ia, w); }, 256);
}
