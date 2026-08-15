#include <catch2/catch_test_macros.hpp>
#include <set>
#include <vector>

#include "jamn_engine/burst_assembler.h"
#include "jamn_engine/dedupe_ring.h"

using jamn::engine::BurstAssembler;
using jamn::engine::DedupeRing;
using jamn::proto::NoteBurst;
using jamn::proto::NoteEvent;

namespace {
NoteEvent MakeEvent(std::uint16_t seq) {
    NoteEvent e;
    e.eventSeq = seq;
    return e;
}

std::set<std::uint16_t> SeqsIn(const NoteBurst& burst) {
    std::set<std::uint16_t> seqs;
    for (std::uint8_t i = 0; i < burst.eventCount; ++i) seqs.insert(burst.events[i].eventSeq);
    return seqs;
}
}  // namespace

TEST_CASE("BurstAssembler with nothing queued builds an empty burst", "[engine][burst_assembler][fast]") {
    BurstAssembler assembler;
    const NoteBurst burst = assembler.BuildNextBurst(0, 0);
    REQUIRE(burst.eventCount == 0);
}

TEST_CASE("BurstAssembler includes a newly queued event in the very next burst",
          "[engine][burst_assembler][fast]") {
    BurstAssembler assembler;
    REQUIRE(assembler.QueueEvent(MakeEvent(1), 0));
    const NoteBurst burst = assembler.BuildNextBurst(0, 0);
    REQUIRE(SeqsIn(burst) == std::set<std::uint16_t>{1});
}

TEST_CASE("With K=3 redundancy, an event rides in exactly 4 consecutive bursts", "[engine][burst_assembler][fast]") {
    BurstAssembler assembler;
    REQUIRE(assembler.QueueEvent(MakeEvent(42), 0));

    for (int i = 0; i < BurstAssembler::kRedundancy + 1; ++i) {
        const NoteBurst burst = assembler.BuildNextBurst(0, static_cast<std::uint16_t>(i));
        INFO("burst " << i);
        REQUIRE(SeqsIn(burst).count(42) == 1);
    }
    // The 5th burst (index kRedundancy+1) no longer carries it - it has
    // fallen out of the redundancy window.
    const NoteBurst fifth = assembler.BuildNextBurst(0, BurstAssembler::kRedundancy + 1);
    REQUIRE(SeqsIn(fifth).count(42) == 0);
}

TEST_CASE("Each new burst combines its own new events with the still-redundant events from earlier bursts",
          "[engine][burst_assembler][fast]") {
    BurstAssembler assembler;
    REQUIRE(assembler.QueueEvent(MakeEvent(1), 0));
    const NoteBurst first = assembler.BuildNextBurst(0, 0);
    REQUIRE(SeqsIn(first) == std::set<std::uint16_t>{1});

    REQUIRE(assembler.QueueEvent(MakeEvent(2), 0));
    const NoteBurst second = assembler.BuildNextBurst(0, 1);
    // Event 1 rides again (redundancy), alongside the newly queued event 2.
    REQUIRE(SeqsIn(second) == (std::set<std::uint16_t>{1, 2}));
}

TEST_CASE("Every redundant copy of an event decodes to the same instant, whatever base its burst carries",
          "[engine][burst_assembler][fast]") {
    // The whole point of QueueEvent taking a time. Each burst stamps its
    // own base_t_session_us, so a copied-verbatim dt_us would place the
    // event later with every burst it rode - and since the receiver dedupes
    // on (peer, event_seq), a lost original means a later copy's wrong
    // timestamp is the one that gets used, with nothing to flag it.
    BurstAssembler assembler;
    constexpr std::int64_t kEventTimeUs = 1'000'000;
    constexpr std::int64_t kBurstPeriodUs = 3000;
    REQUIRE(assembler.QueueEvent(MakeEvent(7), kEventTimeUs));

    for (int i = 0; i < BurstAssembler::kRedundancy + 1; ++i) {
        const std::int64_t baseUs = kEventTimeUs + i * kBurstPeriodUs;
        const NoteBurst burst = assembler.BuildNextBurst(baseUs, static_cast<std::uint16_t>(i));
        INFO("burst " << i << " base " << baseUs);
        bool found = false;
        for (std::uint8_t j = 0; j < burst.eventCount; ++j) {
            if (burst.events[j].eventSeq != 7) continue;
            found = true;
            REQUIRE(baseUs + burst.events[j].dtUs == kEventTimeUs);
        }
        REQUIRE(found);
    }
}

TEST_CASE("K=3 redundancy plus dedupe survives two consecutive burst losses in a row, losing no event",
          "[engine][burst_assembler][dedupe][fast]") {
    BurstAssembler assembler;
    DedupeRing dedupe;
    const jamn::net::PeerId peer = 1;

    // Five cycles: one event queued per cycle, one burst built per cycle.
    // Bursts 1 and 2 (0-indexed) are "lost" - simulated by simply never
    // handing them to the dedupe/delivery path at all.
    std::vector<NoteBurst> bursts;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(assembler.QueueEvent(MakeEvent(static_cast<std::uint16_t>(i)), 0));
        bursts.push_back(assembler.BuildNextBurst(0, static_cast<std::uint16_t>(i)));
    }

    std::set<std::uint16_t> delivered;
    for (std::size_t i = 0; i < bursts.size(); ++i) {
        if (i == 1 || i == 2) continue;  // Two consecutive losses.
        for (std::uint8_t j = 0; j < bursts[i].eventCount; ++j) {
            const auto& event = bursts[i].events[j];
            if (!dedupe.IsDuplicate(peer, event.eventSeq)) {
                delivered.insert(event.eventSeq);
            }
        }
    }

    // Every event queued across all 5 cycles was still delivered exactly
    // once, despite bursts 1 and 2 never arriving - K=3 redundancy means
    // each event's other copies (from bursts 0, 1, 2 and 3, whichever
    // survive) covered the gap.
    REQUIRE(delivered == (std::set<std::uint16_t>{0, 1, 2, 3, 4}));
}
