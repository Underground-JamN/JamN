// T4.2: the net-to-audio crossing.
//
// Every TEST_CASE name here contains the lowercase word "crossing", because
// `ctest -R` matches test *names* rather than Catch2 tags and exits 0 on an
// empty selection - a tag-only marker would make the acceptance command
// silently green.
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include "jamn_core/realtime_scope.h"
#include "jamn_engine/note_crossing.h"

using jamn::core::SetRealtimeViolationHandler;
using jamn::engine::NoteCrossing;
using jamn::proto::NoteEventKind;

namespace {

NoteCrossing::RemoteNote MakeNote(jamn::net::PeerId peer, std::uint16_t seq) {
    NoteCrossing::RemoteNote note;
    note.peer = peer;
    note.remoteSessionTimeUs = 1'000'000 + seq;
    note.event.kind = NoteEventKind::kNoteOn;
    note.event.eventSeq = seq;
    note.event.a = static_cast<std::uint8_t>(seq % 128);
    return note;
}

}  // namespace

TEST_CASE("note crossing hands a published note back in order, on the lane it was published to",
          "[engine][note_crossing][crossing][fast]") {
    NoteCrossing crossing;
    for (std::uint16_t seq = 0; seq < 4; ++seq) {
        REQUIRE(crossing.Publish(2, MakeNote(7, seq)));
    }

    std::vector<std::uint16_t> seqs;
    NoteCrossing::RemoteNote note;
    while (crossing.Consume(2, note)) {
        REQUIRE(note.peer == 7);
        REQUIRE(note.remoteSessionTimeUs == 1'000'000 + note.event.eventSeq);
        seqs.push_back(note.event.eventSeq);
    }
    REQUIRE(seqs == (std::vector<std::uint16_t>{0, 1, 2, 3}));
}

TEST_CASE("note crossing keeps lanes independent, so one peer's backlog is not another's",
          "[engine][note_crossing][crossing][fast]") {
    NoteCrossing crossing;
    // Fill lane 0 to the brim and past it; lane 1 must be untouched by that.
    for (std::size_t i = 0; i < NoteCrossing::kCapacityPerPeer; ++i) {
        REQUIRE(crossing.Publish(0, MakeNote(1, static_cast<std::uint16_t>(i))));
    }
    REQUIRE_FALSE(crossing.Publish(0, MakeNote(1, 999)));

    REQUIRE(crossing.Publish(1, MakeNote(2, 42)));
    NoteCrossing::RemoteNote note;
    REQUIRE(crossing.Consume(1, note));
    REQUIRE(note.peer == 2);
    REQUIRE(note.event.eventSeq == 42);
    REQUIRE(crossing.DroppedCount(1) == 0);
}

TEST_CASE("note crossing drops rather than blocking when a lane is full, and counts what it dropped",
          "[engine][note_crossing][crossing][fast]") {
    NoteCrossing crossing;
    for (std::size_t i = 0; i < NoteCrossing::kCapacityPerPeer; ++i) {
        REQUIRE(crossing.Publish(3, MakeNote(5, static_cast<std::uint16_t>(i))));
    }

    // There is no backpressure to apply toward a real-time consumer, so a
    // full lane can only refuse. It must not block, not grow, and not evict
    // what the audio thread has not read yet.
    for (int i = 0; i < 10; ++i) {
        REQUIRE_FALSE(crossing.Publish(3, MakeNote(5, 900)));
    }
    REQUIRE(crossing.DroppedCount(3) == 10);

    // The oldest note is still the one waiting - nothing already queued was
    // evicted to make room for what got refused.
    NoteCrossing::RemoteNote note;
    REQUIRE(crossing.Consume(3, note));
    REQUIRE(note.event.eventSeq == 0);
}

TEST_CASE("note crossing publishes and consumes a full lane without allocating",
          "[engine][note_crossing][crossing][fast]") {
    // The consume side runs on the audio thread, so it is bound by
    // docs/RT_RULES.md. The publish side is not - it runs on the net
    // thread - but it must not allocate either, since an allocating
    // producer is how a bounded queue quietly stops being bounded.
    NoteCrossing crossing;
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    bool violated = false;
    try {
        jamn::core::RealtimeScope scope;
        for (std::size_t i = 0; i < NoteCrossing::kCapacityPerPeer; ++i) {
            crossing.Publish(4, MakeNote(6, static_cast<std::uint16_t>(i)));
        }
        // Past the brim, which is the path that has to take a refusal
        // branch rather than grow anything.
        for (int i = 0; i < 4; ++i) {
            crossing.Publish(4, MakeNote(6, 800));
        }
        NoteCrossing::RemoteNote note;
        while (crossing.Consume(4, note)) {
        }
    } catch (const std::runtime_error&) {
        violated = true;
    }

    SetRealtimeViolationHandler(nullptr);
    REQUIRE_FALSE(violated);
    REQUIRE(crossing.DroppedCount(4) == 4);
}

TEST_CASE("note crossing carries every note across two real threads, in order and without a race",
          "[engine][note_crossing][crossing][fast]") {
    // The one test in this file that gives `linux-tsan` something to check.
    // Every other case here drives both ends from the test thread, which
    // exercises the logic but presents no concurrency at all - a crossing
    // asserted only that way would pass TSan by never crossing anything.
    //
    // The retry-on-full spin below is a test-thread liberty, not a pattern
    // for the runtime: the real producer drops on a full lane (see the
    // drop-rather-than-block case above) precisely because it may not spin.
    constexpr std::uint16_t kNoteCount = 20'000;
    NoteCrossing crossing;

    std::thread producer([&crossing] {
        for (std::uint16_t seq = 0; seq < kNoteCount; ++seq) {
            while (!crossing.Publish(0, MakeNote(3, seq))) std::this_thread::yield();
        }
    });

    std::vector<std::uint16_t> received;
    received.reserve(kNoteCount);
    std::thread consumer([&crossing, &received] {
        NoteCrossing::RemoteNote note;
        while (received.size() < kNoteCount) {
            if (crossing.Consume(0, note)) {
                received.push_back(note.event.eventSeq);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(received.size() == kNoteCount);
    bool inOrder = true;
    for (std::uint16_t seq = 0; seq < kNoteCount; ++seq) {
        if (received[seq] != seq) inOrder = false;
    }
    REQUIRE(inOrder);
    // DroppedCount is deliberately *not* asserted zero here. It counts
    // refusals, and this producer retries after one - so a nonzero count
    // means the consumer was briefly behind, not that a note was lost. The
    // two readings coincide only in the runtime, which never retries. That
    // every seq arrived exactly once, above, is the real claim.
}

TEST_CASE("note crossing refuses an out-of-range lane instead of running off its array",
          "[engine][note_crossing][crossing][fast]") {
    NoteCrossing crossing;
    NoteCrossing::RemoteNote note;
    REQUIRE_FALSE(crossing.Publish(NoteCrossing::kMaxPeers, MakeNote(1, 0)));
    REQUIRE_FALSE(crossing.Consume(NoteCrossing::kMaxPeers, note));
    REQUIRE(crossing.DroppedCount(NoteCrossing::kMaxPeers) == 0);
    REQUIRE(crossing.DepthApprox(NoteCrossing::kMaxPeers) == 0);
}
