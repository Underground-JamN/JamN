// T4.1: the peer runtime driven over SimTransport, with no ENet and no
// audio device anywhere - which is what lets these carry the `fast` label
// (that list must be identical under core-only and the full build).
//
// Every TEST_CASE name here contains the lowercase word "runtime" on
// purpose: `ctest -R` matches test *names*, not Catch2 tags, is
// case-sensitive, and exits 0 on an empty selection. A name-free tag would
// make the acceptance command silently green.
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

#include "jamn_core/byte_writer.h"
#include "jamn_engine/peer_runtime.h"
#include "jamn_net/sim_transport.h"
#include "jamn_proto/message_type.h"
#include "jamn_proto/note_burst.h"
#include "jamn_proto/note_event.h"
#include "jamn_proto/packet_header.h"
#include "jamn_proto/tlv.h"

using jamn::core::ByteWriter;
using jamn::engine::PeerRuntime;
using jamn::net::Channel;
using jamn::net::LinkConfig;
using jamn::net::PeerId;
using jamn::net::SimNetwork;
using jamn::net::SimTransport;
using jamn::proto::NoteEvent;
using jamn::proto::NoteEventKind;

namespace {

constexpr std::int64_t kStepUs = 1000;
constexpr std::int64_t kLinkDelayUs = 5000;

// Two nodes, a clean symmetric link, and a deliberate clock offset on the
// second one - `b` is driven at (t + clockOffsetUs) while `a` is driven at
// t, which is the only thing that makes "estimate the peer's offset" a real
// question in a sim where both ends could otherwise share one clock.
struct TwoNodes {
    explicit TwoNodes(std::uint64_t seed, std::int64_t clockOffsetUs = 0)
        : net(seed),
          ta(net.CreateNode(0)),
          tb(net.CreateNode(1)),
          a(ta),
          b(tb, clockOffsetUs),
          offsetUs(clockOffsetUs) {
        LinkConfig cfg;
        cfg.delayUs = kLinkDelayUs;
        net.SetLinkConfig(0, 1, cfg);
        net.SetLinkConfig(1, 0, cfg);
        net.Connect(0, 1);
    }

    // Services both runtimes across [nowUs, untilUs], advancing virtual
    // time in lockstep so the sim clock and `a`'s clock read the same.
    void RunUntil(std::int64_t untilUs) {
        for (; nowUs <= untilUs; nowUs += kStepUs) {
            a.Service(nowUs);
            b.Service(nowUs + offsetUs);
            net.Advance(kStepUs);
        }
    }

    SimNetwork net;
    SimTransport& ta;
    SimTransport& tb;
    PeerRuntime a;
    PeerRuntime b;
    std::int64_t offsetUs = 0;
    std::int64_t nowUs = 0;
};

NoteEvent NoteOn(std::uint8_t note) {
    NoteEvent event;
    event.kind = NoteEventKind::kNoteOn;
    event.a = note;
    // Deliberate garbage: the runtime stamps its own sequence numbers, and
    // a caller-supplied one must not survive.
    event.eventSeq = 0xBEEF;
    return event;
}

// The audio thread's side of the crossing, taken from the test thread.
// There is no delivery callback to observe instead - the crossing is the
// runtime's only exit for a remote note, so this is the shipping path.
std::vector<jamn::engine::NoteCrossing::RemoteNote> Drain(PeerRuntime& runtime) {
    std::vector<jamn::engine::NoteCrossing::RemoteNote> drained;
    jamn::engine::NoteCrossing::RemoteNote note;
    for (std::size_t slot = 0; slot < PeerRuntime::kMaxPeers; ++slot) {
        while (runtime.crossing().Consume(slot, note)) drained.push_back(note);
    }
    return drained;
}

std::size_t SlotOf(const PeerRuntime& runtime, PeerId peer) {
    for (std::size_t slot = 0; slot < PeerRuntime::kMaxPeers; ++slot) {
        if (runtime.PeerAt(slot) == peer) return slot;
    }
    return PeerRuntime::kMaxPeers;
}

}  // namespace

TEST_CASE("peer runtime claims a slot per link from the transport's own connect event",
          "[engine][peer_runtime][fast]") {
    TwoNodes nodes(1);
    REQUIRE(nodes.a.PeerCount() == 0);

    nodes.RunUntil(0);  // One Service each: the queued kConnected events land.

    REQUIRE(nodes.a.PeerCount() == 1);
    REQUIRE(nodes.b.PeerCount() == 1);
    REQUIRE(nodes.a.PeerAt(0) == 1);
    REQUIRE(nodes.b.PeerAt(0) == 0);
}

TEST_CASE("peer runtime releases the slot, and the published offset with it, when a link goes down",
          "[engine][peer_runtime][fast]") {
    TwoNodes nodes(2);
    nodes.RunUntil(0);
    REQUIRE(nodes.a.PeerCount() == 1);

    nodes.ta.Disconnect(1);
    nodes.RunUntil(nodes.nowUs + 5 * kStepUs);

    REQUIRE(nodes.a.PeerCount() == 0);
    REQUIRE(nodes.a.PeerAt(0) == PeerRuntime::kNoPeer);
    REQUIRE(nodes.a.ClockSyncFor(1) == nullptr);
}

TEST_CASE("peer runtime delivers a remote note once and dedupes its three redundant copies",
          "[engine][peer_runtime][fast]") {
    TwoNodes nodes(3);

    nodes.RunUntil(0);
    constexpr std::int64_t kSubmitUs = 10'000;
    nodes.RunUntil(kSubmitUs - kStepUs);
    REQUIRE(nodes.a.SubmitLocalEvent(NoteOn(64), kSubmitUs));
    nodes.RunUntil(60'000);

    const auto delivered = Drain(nodes.b);
    REQUIRE(delivered.size() == 1);
    REQUIRE(delivered[0].peer == 0);
    REQUIRE(delivered[0].event.a == 64);
    // Stamped by the runtime, not by the caller's 0xBEEF.
    REQUIRE(delivered[0].event.eventSeq == 0);
    // The invariant BurstAssembler's re-derived dt_us exists to hold: the
    // event decodes to the instant it was submitted, not to the base of
    // whichever of its four copies happened to arrive first.
    REQUIRE(delivered[0].remoteSessionTimeUs == kSubmitUs);
    // K=3: one original plus three repeats, so exactly three copies are
    // discarded by the dedupe ring rather than delivered twice.
    REQUIRE(nodes.b.stats().notesDeduped == 3);
}

TEST_CASE("peer runtime numbers its own outgoing events consecutively from zero",
          "[engine][peer_runtime][fast]") {
    TwoNodes nodes(4);
    nodes.RunUntil(0);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(nodes.a.SubmitLocalEvent(NoteOn(static_cast<std::uint8_t>(60 + i)), nodes.nowUs));
        nodes.RunUntil(nodes.nowUs + 10 * kStepUs);
    }
    nodes.RunUntil(nodes.nowUs + 60 * kStepUs);

    std::vector<std::uint16_t> seqs;
    for (const auto& note : Drain(nodes.b)) seqs.push_back(note.event.eventSeq);
    REQUIRE(seqs == (std::vector<std::uint16_t>{0, 1, 2, 3}));
}

TEST_CASE("peer runtime drops a submitted event rather than blocking when the local ring is full",
          "[engine][peer_runtime][fast]") {
    TwoNodes nodes(5);
    // Nothing is serviced, so nothing drains: the ring fills and the next
    // push is refused. A full ring must drop - it may not block, allocate,
    // or overwrite the reader's tail.
    for (std::size_t i = 0; i < PeerRuntime::kLocalEventCapacity; ++i) {
        REQUIRE(nodes.a.SubmitLocalEvent(NoteOn(60), 0));
    }
    REQUIRE_FALSE(nodes.a.SubmitLocalEvent(NoteOn(60), 0));
}

TEST_CASE("peer runtime drops realtime traffic from a link that holds no slot",
          "[engine][peer_runtime][fast]") {
    SimNetwork net(6);
    SimTransport& ta = net.CreateNode(0);
    SimTransport& tstranger = net.CreateNode(9);
    PeerRuntime a(ta);

    // A well-formed, entirely valid NoteBurst packet - from a link that has
    // never connected. It must be dropped on the link identity alone,
    // before a single field of it is decoded.
    std::array<std::uint8_t, 64> value{};
    ByteWriter valueWriter(value.data(), value.size());
    jamn::proto::NoteBurst burst;
    burst.eventCount = 1;
    burst.events[0] = NoteOn(70);
    REQUIRE(jamn::proto::EncodeNoteBurst(burst, valueWriter));

    std::array<std::uint8_t, 128> packet{};
    ByteWriter packetWriter(packet.data(), packet.size());
    jamn::proto::PacketHeader header;
    header.bodyLen = static_cast<std::uint16_t>(4 + valueWriter.Position());
    REQUIRE(jamn::proto::EncodePacketHeader(header, packetWriter));
    REQUIRE(jamn::proto::WriteTlvHeader(packetWriter, static_cast<std::uint16_t>(jamn::proto::MessageType::kNoteBurst),
                                        static_cast<std::uint16_t>(valueWriter.Position())));
    REQUIRE(packetWriter.WriteBytes(value.data(), valueWriter.Position()));

    REQUIRE(tstranger.Send(0, Channel::kRealtime, packet.data(), packetWriter.Position()));
    net.Advance(kStepUs);
    a.Service(kStepUs);

    REQUIRE(Drain(a).empty());
    REQUIRE(a.stats().packetsFromUnknownPeer == 1);
    REQUIRE(a.stats().notesDelivered == 0);
}

TEST_CASE("peer runtime hands a control packet out whole and undecoded", "[engine][peer_runtime][fast]") {
    TwoNodes nodes(7);
    std::vector<std::uint8_t> seen;
    nodes.b.SetControlPacketCallback([&seen](PeerId, jamn::core::ByteReader& packet) {
        seen.resize(packet.Remaining());
        if (!seen.empty()) REQUIRE(packet.ReadBytes(seen.data(), seen.size()));
    });

    // Deliberately not a packet this runtime could decode - the point is
    // that it does not try. Framing and meaning are the session layer's.
    const std::array<std::uint8_t, 5> payload{0x01, 0x02, 0x03, 0x04, 0x05};
    REQUIRE(nodes.ta.Send(1, Channel::kControl, payload.data(), payload.size()));
    nodes.RunUntil(8 * kStepUs);

    REQUIRE(seen == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    REQUIRE(nodes.b.stats().packetsRejected == 0);
}

TEST_CASE("peer runtime pings, answers a peer's ping with a pong, and locks that peer's offset",
          "[engine][peer_runtime][fast]") {
    constexpr std::int64_t kClockOffsetUs = 40'000;
    TwoNodes nodes(8, kClockOffsetUs);

    // kLockThreshold samples at the 4Hz fast-phase cadence, plus a round
    // trip for the last one to come back.
    nodes.RunUntil(2'000'000);

    const std::size_t slot = SlotOf(nodes.a, 1);
    REQUIRE(slot < PeerRuntime::kMaxPeers);

    const jamn::engine::ClockSync* sync = nodes.a.ClockSyncFor(1);
    REQUIRE(sync != nullptr);
    REQUIRE(sync->IsLocked());
    REQUIRE(nodes.a.stats().pingsSent > 0);
    REQUIRE(nodes.b.stats().pongsSent > 0);
    REQUIRE(nodes.a.stats().clockSamplesFolded >= jamn::engine::ClockSync::kLockThreshold);

    // A symmetric link with no jitter: the estimate should be the offset,
    // not merely near it.
    REQUIRE(nodes.a.PublishedOffsetUs(slot) == kClockOffsetUs);
    // ...and the reverse direction sees its negation, which is the check
    // that would fail if t2/t3 were filled from the wrong end's clock.
    const std::size_t slotB = SlotOf(nodes.b, 0);
    REQUIRE(slotB < PeerRuntime::kMaxPeers);
    REQUIRE(nodes.b.PublishedOffsetUs(slotB) == -kClockOffsetUs);
}

TEST_CASE("peer runtime says whether an offset is locked, since a zero offset is a real answer",
          "[engine][peer_runtime][fast]") {
    // Two peers on one machine genuinely have a zero offset, so the value
    // alone cannot distinguish "the correction is zero" from "there is no
    // correction yet" - which is exactly the case the loopback clock bench
    // measures, and would have scored an unlocked slot as a perfect one.
    TwoNodes nodes(10, /*clockOffsetUs=*/0);
    nodes.RunUntil(0);

    const std::size_t slot = SlotOf(nodes.a, 1);
    REQUIRE(slot < PeerRuntime::kMaxPeers);
    REQUIRE(nodes.a.PublishedOffsetUs(slot) == 0);
    REQUIRE_FALSE(nodes.a.OffsetIsLocked(slot));

    nodes.RunUntil(2'000'000);

    REQUIRE(nodes.a.OffsetIsLocked(slot));
    // Same reading as before it locked, and now it means something.
    REQUIRE(nodes.a.PublishedOffsetUs(slot) == 0);
}

TEST_CASE("peer runtime reports a clock re-lock as a generation bump, never as scheduler state",
          "[engine][peer_runtime][fast]") {
    constexpr std::int64_t kClockOffsetUs = 40'000;
    TwoNodes nodes(9, kClockOffsetUs);
    nodes.RunUntil(2'000'000);

    const std::size_t slot = SlotOf(nodes.a, 1);
    REQUIRE(slot < PeerRuntime::kMaxPeers);
    REQUIRE(nodes.a.ReLockGeneration(slot) == 0);

    // A discontinuity well past kReLockThresholdUs - a peer's machine
    // waking from sleep, or its host restarting.
    nodes.offsetUs += 1'000'000;
    nodes.RunUntil(nodes.nowUs + 3'000'000);

    REQUIRE(nodes.a.ReLockGeneration(slot) >= 1);
    // The counter is the whole of the net thread's response. Flushing held
    // notes is audio-thread work, so nothing here reached into a scheduler.
    REQUIRE(nodes.a.PeerCount() == 1);
}
