#include <catch2/catch_test_macros.hpp>

#include "jamn_engine/jitter_buffer.h"

using jamn::engine::JitterBuffer;

TEST_CASE("JitterBuffer with no samples targets just the safety margin", "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    REQUIRE(jb.PlayoutTargetUs(0) == JitterBuffer::kSafetyMarginUs);
}

TEST_CASE("JitterBuffer's target tracks P99(transit) + 3ms", "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    for (int i = 0; i < 100; ++i) {
        jb.RecordTransit(10'000, 0);  // 10ms transit, tightly clustered.
    }
    const std::int64_t target = jb.PlayoutTargetUs(0);
    REQUIRE(target > 10'000);
    REQUIRE(target < 15'000);
}

TEST_CASE("JitterBuffer clamps its target to 200ms", "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    for (int i = 0; i < 100; ++i) {
        jb.RecordTransit(1'000'000, 0);  // Absurdly large transit time.
    }
    REQUIRE(jb.PlayoutTargetUs(0) == JitterBuffer::kMaxPlayoutUs);
}

TEST_CASE("A single late event raises the playout target immediately", "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    REQUIRE(jb.PlayoutTargetUs(0) == JitterBuffer::kSafetyMarginUs);

    for (int i = 0; i < 20; ++i) {
        jb.RecordTransit(50'000, 0);  // A late arrival implies a bigger transit time than seen before.
    }
    // No call to PlayoutTargetUs() in between - ReportLateEvent itself must
    // apply the jump, not wait for the next poll.
    jb.ReportLateEvent(0);
    REQUIRE(jb.PlayoutTargetUs(0) > JitterBuffer::kSafetyMarginUs);
    REQUIRE(jb.PlayoutTargetUs(0) >= 50'000);
}

TEST_CASE("The target does not shrink at the next evaluation while a late event's grace is still owed",
          "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    // Establish a high target via a late event.
    for (int i = 0; i < 20; ++i) jb.RecordTransit(50'000, 0);
    jb.ReportLateEvent(0);
    const std::int64_t highTarget = jb.PlayoutTargetUs(0);
    REQUIRE(jb.LateEventGraceOwed() == 1);

    // One elapsed shrink interval later, with the underlying transit
    // distribution now low (so the *computed* target alone would be much
    // lower): grace is still owed for exactly this one evaluation, so the
    // target must not move yet, and the owed grace is consumed by it.
    jb.RecordTransit(0, JitterBuffer::kShrinkIntervalUs);
    const std::int64_t target = jb.PlayoutTargetUs(JitterBuffer::kShrinkIntervalUs);
    REQUIRE(target == highTarget);
    REQUIRE(jb.LateEventGraceOwed() == 0);
}

TEST_CASE("JitterBuffer shrinks by at most 1ms per elapsed 2-second interval once grace is exhausted",
          "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    for (int i = 0; i < 20; ++i) jb.RecordTransit(50'000, 0);
    jb.ReportLateEvent(0);
    const std::int64_t highTarget = jb.PlayoutTargetUs(0);
    REQUIRE(jb.LateEventGraceOwed() == 1);

    // Burn off the one owed grace interval first (still no shrink here).
    std::int64_t t = JitterBuffer::kShrinkIntervalUs;
    jb.RecordTransit(0, t);
    REQUIRE(jb.PlayoutTargetUs(t) == highTarget);
    REQUIRE(jb.LateEventGraceOwed() == 0);

    // From here, keep feeding low transit samples every interval. The
    // target must never increase, and must never drop by more than
    // kShrinkStepUs in a single evaluation, regardless of how the
    // underlying P99 computation moves as the histogram window evolves.
    std::int64_t previous = highTarget;
    for (int i = 0; i < 6; ++i) {
        t += JitterBuffer::kShrinkIntervalUs;
        jb.RecordTransit(0, t);
        const std::int64_t current = jb.PlayoutTargetUs(t);
        REQUIRE(current <= previous);
        REQUIRE(previous - current <= JitterBuffer::kShrinkStepUs);
        previous = current;
    }
    // After the original high-transit samples have long aged out of the
    // 5-second window (6 * 2s = 12s of low-transit-only samples), the
    // target should have shrunk meaningfully from where it started.
    REQUIRE(previous < highTarget - 3 * JitterBuffer::kShrinkStepUs);
}

TEST_CASE("JitterBuffer does not shrink before a full interval has elapsed", "[engine][jitter_buffer][fast]") {
    JitterBuffer jb;
    for (int i = 0; i < 20; ++i) jb.RecordTransit(50'000, 0);
    jb.ReportLateEvent(0);
    const std::int64_t highTarget = jb.PlayoutTargetUs(0);

    // Burn the owed grace at t = one interval.
    std::int64_t t = JitterBuffer::kShrinkIntervalUs;
    jb.RecordTransit(0, t);
    jb.PlayoutTargetUs(t);
    REQUIRE(jb.LateEventGraceOwed() == 0);

    // Less than a full interval later - must not have shrunk yet.
    jb.RecordTransit(0, t + 100);
    REQUIRE(jb.PlayoutTargetUs(t + 100) == highTarget);
}
