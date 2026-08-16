#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "jamn_core/realtime_scope.h"
#include "jamn_engine/deadline_resolver.h"
#include "jamn_engine/event_scheduler.h"

using jamn::core::RealtimeScope;
using jamn::core::SetRealtimeViolationHandler;
using jamn::engine::EventScheduler;
using jamn::engine::IDeadlineResolver;
using jamn::engine::LiveResolver;
using jamn::engine::MusicalResolver;
using jamn::net::PeerId;
using jamn::proto::NoteEvent;
using jamn::proto::NoteEventKind;

namespace {
NoteEvent MakeEvent(NoteEventKind kind, std::uint16_t stateRev = 0) {
    NoteEvent e;
    e.kind = kind;
    e.stateRev = stateRev;
    return e;
}
}  // namespace

TEST_CASE("EventScheduler refuses to install a resolver whose implemented() is false",
          "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    MusicalResolver musical;
    REQUIRE_FALSE(musical.implemented());
    REQUIRE_FALSE(scheduler.SetResolver(&musical));

    LiveResolver live;
    REQUIRE(live.implemented());
    REQUIRE(scheduler.SetResolver(&live));
}

TEST_CASE("EventScheduler comes with LiveResolver installed by default", "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    // No SetResolver call at all - scheduling a remote event must still
    // work, proving LiveResolver is live from the start (not requiring the
    // caller to install one first).
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOn);
    REQUIRE(scheduler.ScheduleRemoteEvent(/*peer=*/1, e, /*eventSessionTimeUs=*/1000, /*nowUs=*/1000));
}

TEST_CASE("Local input is always scheduled at zero added delay", "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOn);
    REQUIRE(scheduler.ScheduleLocalEvent(e, /*nowUs=*/500'000));

    // Not ready one microsecond before now...
    EventScheduler::Delivery delivery;
    REQUIRE_FALSE(scheduler.PopReady(499'999, delivery));
    // ...but ready exactly at now - zero added delay, regardless of
    // whatever jitter-buffer playout delay a remote event would carry.
    REQUIRE(scheduler.PopReady(500'000, delivery));
    REQUIRE(delivery.peer == EventScheduler::kLocalPeerId);
}

TEST_CASE("A note-on 10ms late is dropped and the jitter target grows", "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const PeerId peer = 1;
    const std::int64_t before = scheduler.JitterBufferFor(peer).PlayoutTargetUs(0);

    // eventSessionTimeUs=0, nowUs=10000: with no transit history yet, the
    // jitter buffer's playout delay is just its 3ms safety margin, so the
    // computed deadline is 3000 - processing this at nowUs=10000 makes it
    // 7ms late, at/above the 5ms drop threshold.
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOn);
    const bool accepted = scheduler.ScheduleRemoteEvent(peer, e, /*eventSessionTimeUs=*/0, /*nowUs=*/10'000);

    REQUIRE_FALSE(accepted);
    REQUIRE(scheduler.ScheduledCount() == 0);
    const std::int64_t after = scheduler.JitterBufferFor(peer).PlayoutTargetUs(10'000);
    REQUIRE(after > before);
}

TEST_CASE("A note-off 50ms late is still played", "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const PeerId peer = 1;
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOff);
    const bool accepted = scheduler.ScheduleRemoteEvent(peer, e, /*eventSessionTimeUs=*/0, /*nowUs=*/50'000);

    REQUIRE(accepted);
    REQUIRE(scheduler.ScheduledCount() == 1);

    EventScheduler::Delivery delivery;
    REQUIRE(scheduler.PopReady(50'000, delivery));
    REQUIRE(delivery.event.kind == NoteEventKind::kNoteOff);
}

TEST_CASE("An all-notes-off event is never dropped, however late", "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const NoteEvent e = MakeEvent(NoteEventKind::kAllNotesOff);
    REQUIRE(scheduler.ScheduleRemoteEvent(/*peer=*/1, e, /*eventSessionTimeUs=*/0, /*nowUs=*/1'000'000));
    REQUIRE(scheduler.ScheduledCount() == 1);
}

TEST_CASE("A remote event less than 5ms late still plays, at sample 0 rather than in the past",
          "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOn);
    // Deadline is eventSessionTimeUs(0) + the default 3ms playout margin =
    // 3000; processing at nowUs=6000 makes it 3ms late - under the 5ms
    // drop threshold.
    REQUIRE(scheduler.ScheduleRemoteEvent(/*peer=*/1, e, /*eventSessionTimeUs=*/0, /*nowUs=*/6'000));

    EventScheduler::Delivery delivery;
    REQUIRE(scheduler.PopReady(6'000, delivery));
}

TEST_CASE("PopReady delivers events in deadline order regardless of scheduling order",
          "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    // Three local events scheduled out of deadline order.
    NoteEvent a = MakeEvent(NoteEventKind::kNoteOn);
    a.eventSeq = 1;
    NoteEvent b = MakeEvent(NoteEventKind::kNoteOn);
    b.eventSeq = 2;
    NoteEvent c = MakeEvent(NoteEventKind::kNoteOn);
    c.eventSeq = 3;

    REQUIRE(scheduler.ScheduleLocalEvent(b, 2000));
    REQUIRE(scheduler.ScheduleLocalEvent(c, 3000));
    REQUIRE(scheduler.ScheduleLocalEvent(a, 1000));

    EventScheduler::Delivery delivery;
    REQUIRE(scheduler.PopReady(3000, delivery));
    REQUIRE(delivery.event.eventSeq == 1);
    REQUIRE(scheduler.PopReady(3000, delivery));
    REQUIRE(delivery.event.eventSeq == 2);
    REQUIRE(scheduler.PopReady(3000, delivery));
    REQUIRE(delivery.event.eventSeq == 3);
    REQUIRE_FALSE(scheduler.PopReady(3000, delivery));
}

TEST_CASE("PopReady delivers events that tie on deadline in the order they arrived",
          "[engine][event_scheduler][fast]") {
    // The companion to the case above, and the one a real bug came from:
    // every local event scheduled in one audio block carries that block's
    // single `now`, so ties are the normal case for local input, not an
    // edge one. std::push_heap/pop_heap are not stable, so without an
    // explicit tie-break a note's off could be delivered before its own
    // on - which sounds forever, because nothing switches it off again.
    // Found by dragging the mouse fast across the on-screen piano.
    EventScheduler scheduler;

    constexpr std::uint16_t kCount = 12;
    for (std::uint16_t index = 0; index < kCount; ++index) {
        NoteEvent event = MakeEvent(index % 2 == 0 ? NoteEventKind::kNoteOn : NoteEventKind::kNoteOff);
        event.eventSeq = index;
        REQUIRE(scheduler.ScheduleLocalEvent(event, 5000));
    }

    // Twelve, not two: a two-element heap happens to come out in order
    // whatever the comparator does, so a smaller case would pass against
    // the very bug this pins.
    EventScheduler::Delivery delivery;
    for (std::uint16_t index = 0; index < kCount; ++index) {
        REQUIRE(scheduler.PopReady(5000, delivery));
        REQUIRE(delivery.event.eventSeq == index);
    }
    REQUIRE_FALSE(scheduler.PopReady(5000, delivery));
}

TEST_CASE("An event whose state_rev is ahead of local_rev is held, not dropped or played early",
          "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const PeerId peer = 1;
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOn, /*stateRev=*/5);

    REQUIRE(scheduler.ScheduleRemoteEvent(peer, e, /*eventSessionTimeUs=*/0, /*nowUs=*/0));
    REQUIRE(scheduler.HeldCount() == 1);
    REQUIRE(scheduler.ScheduledCount() == 0);

    EventScheduler::Delivery delivery;
    REQUIRE_FALSE(scheduler.PopReady(0, delivery));

    // Advancing local_rev to catch up releases it on the next PopReady -
    // polled generously past the default 3ms playout margin so the
    // now-released event (which still goes through the normal deadline
    // computation) is actually due.
    scheduler.SetLocalRev(peer, 5);
    REQUIRE(scheduler.PopReady(10'000, delivery));
    REQUIRE(scheduler.HeldCount() == 0);
}

TEST_CASE("A held event plays anyway after the 100ms hold timeout even if local_rev never catches up",
          "[engine][event_scheduler][fast]") {
    EventScheduler scheduler;
    const PeerId peer = 1;
    const NoteEvent e = MakeEvent(NoteEventKind::kNoteOn, /*stateRev=*/99);

    REQUIRE(scheduler.ScheduleRemoteEvent(peer, e, /*eventSessionTimeUs=*/0, /*nowUs=*/0));
    REQUIRE(scheduler.HeldCount() == 1);

    EventScheduler::Delivery delivery;
    // Still held just before the timeout.
    REQUIRE_FALSE(scheduler.PopReady(EventScheduler::kStateRevHoldUs - 1, delivery));
    REQUIRE(scheduler.HeldCount() == 1);

    // At/after the timeout, it releases and plays regardless of local_rev.
    REQUIRE(scheduler.PopReady(EventScheduler::kStateRevHoldUs, delivery));
    REQUIRE(scheduler.HeldCount() == 0);
}

TEST_CASE("EventScheduler's real-time guard is actually armed in this binary", "[engine][event_scheduler][fast]") {
    SetRealtimeViolationHandler([](const char*) { throw std::runtime_error("rt violation"); });

    bool reported = false;
    volatile int sink = 0;
    try {
        RealtimeScope scope;
        int* value = new int(5);
        sink = *value;
        delete value;
    } catch (const std::runtime_error&) {
        reported = true;
    }
    (void)sink;

    SetRealtimeViolationHandler(nullptr);
    REQUIRE(reported);
}

TEST_CASE("FlushAll discards every queued and held event, for every peer",
          "[engine][event_scheduler][fast]") {
    // What panic needs and FlushPeer cannot express: a player reaching
    // for it has no peer in mind.
    EventScheduler scheduler;
    const PeerId peer = 7;
    scheduler.SetLocalRev(peer, 0);

    REQUIRE(scheduler.ScheduleLocalEvent(MakeEvent(NoteEventKind::kNoteOn), 1000));
    REQUIRE(scheduler.ScheduleRemoteEvent(peer, MakeEvent(NoteEventKind::kNoteOn), 1000, 1000));
    // One held for a state_rev it has not seen, so both stores are
    // non-empty when the flush lands.
    REQUIRE(scheduler.ScheduleRemoteEvent(peer, MakeEvent(NoteEventKind::kNoteOn, /*stateRev=*/5), 1000, 1000));
    REQUIRE(scheduler.ScheduledCount() == 2);
    REQUIRE(scheduler.HeldCount() == 1);

    REQUIRE(scheduler.FlushAll() == 3);
    REQUIRE(scheduler.ScheduledCount() == 0);
    REQUIRE(scheduler.HeldCount() == 0);

    EventScheduler::Delivery delivery;
    REQUIRE_FALSE(scheduler.PopReady(200'000, delivery));

    // Still usable afterwards - panic is not a teardown.
    REQUIRE(scheduler.ScheduleLocalEvent(MakeEvent(NoteEventKind::kNoteOn), 2000));
    REQUIRE(scheduler.PopReady(2000, delivery));
}
