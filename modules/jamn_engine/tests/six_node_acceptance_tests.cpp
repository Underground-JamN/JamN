// The six-node sim acceptance suite (PHASE_0_5_PLAN.md's criterion #2
// reading). Exercises SimTransport + the peer runtime together at scale: 6
// nodes, 10 virtual minutes, 2% loss, 30+-10ms jitter, and the reordering
// that jitter alone produces. The claim this produces is "zero stuck notes
// for every seed in the committed corpus, and a failing seed gets committed
// to that corpus" - not an unconditional guarantee (see this plan's
// Decisions section) - so a failure here should be root-caused and, if it
// reflects a real gap in the redundancy/dedupe design rather than a test
// bug, committed as a regression seed in modules/jamn_net/tests/sim_seeds/.
//
// **This suite drives PeerRuntime; it does not reimplement it.** It used to
// assemble bursts, number events, frame packets and dedupe deliveries
// itself, which meant a green run proved the test's own copy of the run
// loop worked - not the one that ships. Everything below the note-generation
// pattern is now the production path: submit local input, service the
// runtime, observe what comes out the other end.
//
// **The determinism claim below is SimTransport-scoped, and must not be
// carried forward to a real socket.** A byte-identical deliveryLog across
// two runs of one seed is a property of SimNetwork's queue - it delivers in
// ascending arrival time, ties broken by send sequence. EnetTransport
// delivers in whatever order the socket hands enet_host_service, so the
// two-process loopback checkpoint asserts zero stuck notes, never an
// identical log.
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "jamn_engine/note_crossing.h"
#include "jamn_engine/peer_runtime.h"
#include "jamn_net/sim_transport.h"
#include "jamn_proto/note_event.h"

using namespace jamn::core;
using namespace jamn::net;
using namespace jamn::proto;
using jamn::engine::PeerRuntime;

namespace {

constexpr int kNumNodes = 6;
constexpr std::int64_t kTickUs = 50'000;
constexpr std::int64_t kSimDurationUs = 10 * 60 * 1'000'000LL;  // 10 virtual minutes.
constexpr std::int64_t kDrainDurationUs = 5'000'000;            // Let trailing K=3 copies and note-offs flush.
constexpr std::int64_t kNoteOnIntervalUs = 500'000;
constexpr std::int64_t kNoteOffDelayUs = 150'000;
constexpr double kLossProbability = 0.02;
constexpr std::int64_t kJitterBaseUs = 30'000;
constexpr std::int64_t kJitterSpreadUs = 10'000;

struct SixNodeResult {
    // Any runtime-reported fault at all, not just a decode one: a rejected
    // packet, a packet from an unknown link, a dropped local event, an
    // oversized burst, a refused send. Named for what it covers, because a
    // future ring-capacity or burst-size regression surfacing under a
    // decode-shaped name in a suite about stuck notes would be a diagnosis
    // problem, not a detection one.
    bool anyRuntimeFault = false;
    std::uint64_t totalDelivered = 0;
    std::uint64_t totalNoteOns = 0;
    std::uint64_t totalNoteOffs = 0;
    // [receivingNode] -> (senderPeer, noteId) pairs still "on" with no
    // matching "off" observed by the end of the run.
    std::array<std::set<std::pair<PeerId, std::uint8_t>>, kNumNodes> stuckNotes;
    // Every (receivingNode, from, event_seq) tuple actually delivered
    // (post-dedupe), in delivery order - used for the determinism check.
    std::vector<std::tuple<PeerId, PeerId, std::uint16_t>> deliveryLog;
};

SixNodeResult RunSixNodeSim(std::uint64_t seed) {
    SixNodeResult result;

    SimNetwork net(seed);
    std::array<SimTransport*, kNumNodes> transports{};
    for (PeerId i = 0; i < kNumNodes; ++i) {
        transports[i] = &net.CreateNode(i);
    }
    for (PeerId i = 0; i < kNumNodes; ++i) {
        for (PeerId j = 0; j < kNumNodes; ++j) {
            if (i == j) continue;
            LinkConfig cfg;
            cfg.delayUs = kJitterBaseUs;
            cfg.jitterUs = kJitterSpreadUs;
            cfg.lossProbability = kLossProbability;
            net.SetLinkConfig(i, j, cfg);
        }
    }

    std::vector<std::unique_ptr<PeerRuntime>> runtimes;
    runtimes.reserve(kNumNodes);
    for (PeerId node = 0; node < kNumNodes; ++node) {
        auto runtime = std::make_unique<PeerRuntime>(*transports[node]);
        // One burst per tick, matching the cadence this sim advances at.
        runtime->SetBurstPeriodUs(kTickUs);
        runtimes.push_back(std::move(runtime));
    }

    // Every node meets every other. The runtimes claim their peer slots from
    // these events on their first Service, so no node is configured with a
    // peer table the transport didn't hand it.
    for (PeerId i = 0; i < kNumNodes; ++i) {
        for (PeerId j = static_cast<PeerId>(i + 1); j < kNumNodes; ++j) {
            net.Connect(i, j);
        }
    }

    std::array<std::uint8_t, kNumNodes> noteCounter{};
    std::array<std::int64_t, kNumNodes> nextNoteOnAtUs{};
    struct PendingOff {
        std::int64_t dueAtUs;
        std::uint8_t noteId;
    };
    std::array<std::optional<PendingOff>, kNumNodes> pendingOff{};

    for (PeerId node = 0; node < kNumNodes; ++node) {
        nextNoteOnAtUs[node] = node * (kNoteOnIntervalUs / kNumNodes);  // Stagger start phases.
    }

    auto tick = [&](std::int64_t nowUs, bool allowNewNotes) {
        for (PeerId node = 0; node < kNumNodes; ++node) {
            if (allowNewNotes && nowUs >= nextNoteOnAtUs[node]) {
                NoteEvent on;
                on.kind = NoteEventKind::kNoteOn;
                on.a = static_cast<std::uint8_t>(noteCounter[node]++ % 128);
                runtimes[node]->SubmitLocalEvent(on, nowUs);
                pendingOff[node] = PendingOff{nowUs + kNoteOffDelayUs, on.a};
                nextNoteOnAtUs[node] = nowUs + kNoteOnIntervalUs;
            }
            if (pendingOff[node].has_value() && nowUs >= pendingOff[node]->dueAtUs) {
                NoteEvent off;
                off.kind = NoteEventKind::kNoteOff;
                off.a = pendingOff[node]->noteId;
                runtimes[node]->SubmitLocalEvent(off, nowUs);
                pendingOff[node].reset();
            }

            // Poll, drain, ping, assemble, broadcast - all of it, in the
            // order the shipping runtime does it.
            runtimes[node]->Service(nowUs);

            // Then take the audio thread's side of the crossing. Standing in
            // for a block boundary: the same lanes, drained the same way,
            // just from this single-threaded driver rather than a callback.
            jamn::engine::NoteCrossing& crossing = runtimes[node]->crossing();
            for (std::size_t slot = 0; slot < PeerRuntime::kMaxPeers; ++slot) {
                jamn::engine::NoteCrossing::RemoteNote note;
                while (crossing.Consume(slot, note)) {
                    ++result.totalDelivered;
                    result.deliveryLog.emplace_back(node, note.peer, note.event.eventSeq);

                    const auto noteKey = std::make_pair(note.peer, note.event.a);
                    if (note.event.kind == NoteEventKind::kNoteOn) {
                        ++result.totalNoteOns;
                        result.stuckNotes[node].insert(noteKey);
                    } else if (note.event.kind == NoteEventKind::kNoteOff) {
                        ++result.totalNoteOffs;
                        result.stuckNotes[node].erase(noteKey);
                    }
                }
            }
        }
        net.Advance(kTickUs);
    };

    std::int64_t nowUs = 0;
    while (nowUs < kSimDurationUs) {
        tick(nowUs, /*allowNewNotes=*/true);
        nowUs += kTickUs;
    }
    const std::int64_t drainUntilUs = nowUs + kDrainDurationUs;
    while (nowUs < drainUntilUs) {
        tick(nowUs, /*allowNewNotes=*/false);
        nowUs += kTickUs;
    }

    for (PeerId node = 0; node < kNumNodes; ++node) {
        const PeerRuntime::Stats& stats = runtimes[node]->stats();
        if (stats.packetsRejected > 0 || stats.packetsFromUnknownPeer > 0 || stats.localEventsDropped > 0 ||
            stats.packetsTooLarge > 0 || stats.sendsFailed > 0 || stats.notesDroppedAtCrossing > 0) {
            result.anyRuntimeFault = true;
        }
    }

    return result;
}

}  // namespace

TEST_CASE("Six-node sim: every note-on has a matching note-off at every node, zero runtime faults",
          "[engine][six_node_sim][fast]") {
    const SixNodeResult result = RunSixNodeSim(/*seed=*/0xC0FFEE);

    REQUIRE_FALSE(result.anyRuntimeFault);
    REQUIRE(result.totalDelivered > 0);
    REQUIRE(result.totalNoteOns > 0);
    REQUIRE(result.totalNoteOffs > 0);

    for (int node = 0; node < kNumNodes; ++node) {
        INFO("node " << node << " has " << result.stuckNotes[node].size() << " stuck note(s)");
        REQUIRE(result.stuckNotes[node].empty());
    }
}

TEST_CASE("Six-node sim: the same seed produces byte-identical delivery results across two runs",
          "[engine][six_node_sim][fast]") {
    const SixNodeResult first = RunSixNodeSim(/*seed=*/0xABCDEF01);
    const SixNodeResult second = RunSixNodeSim(/*seed=*/0xABCDEF01);

    REQUIRE(first.totalDelivered == second.totalDelivered);
    REQUIRE(first.totalNoteOns == second.totalNoteOns);
    REQUIRE(first.totalNoteOffs == second.totalNoteOffs);
    REQUIRE(first.deliveryLog == second.deliveryLog);
    REQUIRE(first.stuckNotes == second.stuckNotes);
}

TEST_CASE("Six-node sim: a small seed sweep beyond the nominal seed also has zero stuck notes",
          "[engine][six_node_sim][fast]") {
    // Not exhaustive - a genuinely failing seed found here or elsewhere
    // gets committed to modules/jamn_net/tests/sim_seeds/ as a permanent
    // regression case per that directory's README, not silently noted.
    for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{0x5EED5EED}}) {
        const SixNodeResult result = RunSixNodeSim(seed);
        INFO("seed " << seed);
        REQUIRE_FALSE(result.anyRuntimeFault);
        for (int node = 0; node < kNumNodes; ++node) {
            REQUIRE(result.stuckNotes[node].empty());
        }
    }
}
