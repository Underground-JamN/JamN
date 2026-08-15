#pragma once

#include <cstdint>

#include "jamn_core/histogram64.h"

namespace jamn::engine {

// Per-peer adaptive playout delay: playout = clamp(P99(transit) + 3ms, 0,
// 200ms), computed over a 5-second window via jamn_core::Histogram64
// (ARCHITECTURE_PLAN.md's jitter-buffer section). Growth is instant - a
// single late event jumps the target up right away - but shrinking is
// slow and gated: at most 1ms per 2 seconds, and only while no late events
// are still "owed" a grace period. This asymmetry is deliberate: jumping
// down as fast as it jumps up would oscillate against the same jitter that
// caused the growth in the first place.
class JitterBuffer {
public:
    static constexpr std::int64_t kSafetyMarginUs = 3000;
    static constexpr std::int64_t kMaxPlayoutUs = 200'000;
    static constexpr std::int64_t kShrinkStepUs = 1000;
    static constexpr std::int64_t kShrinkIntervalUs = 2'000'000;

    // A floor under the playout target, in microseconds - normally the
    // audio block period, set by whoever owns the scheduler.
    //
    // It exists because the audio thread checks deadlines once per block,
    // not continuously, so an event's deadline can already be up to a full
    // block period stale by the time anything looks at it. With a target
    // of only kSafetyMarginUs and a block period several times that, the
    // *first* remote note of a session is therefore late on arrival
    // through no fault of the network, and gets dropped by
    // EventScheduler's late-event policy. The buffer then grows and every
    // later note is fine - so the symptom is exactly one lost note per
    // session, which is easy to see on hardware and invisible in a sim
    // where nothing imposes a block period. Measured on the dev box at 512
    // frames / 44.1kHz: an 11.6ms block against a 3ms target.
    //
    // A floor rather than an addition: the target must *reach* one block
    // period so a deadline cannot already be stale, but adding a block
    // period on top of a healthy P99 would buy nothing and cost real
    // latency in a budget measured in low tens of milliseconds.
    void SetMinTargetUs(std::int64_t minTargetUs) { minTargetUs_ = minTargetUs; }
    std::int64_t MinTargetUs() const { return minTargetUs_; }

    // Feeds one observed transit time (arrival time minus send time,
    // microseconds) into the P99 histogram.
    void RecordTransit(std::int64_t transitUs, std::int64_t nowUs) { histogram_.Record(transitUs, nowUs); }

    // Reports that an event arrived later than the current playout target
    // could accommodate. Jumps the target up immediately to the current
    // clamp(P99+3ms, 0, 200ms) if that's higher than the current target,
    // and marks one shrink-interval's worth of grace as owed before
    // shrinking can resume.
    void ReportLateEvent(std::int64_t nowUs);

    // Returns the current playout target, first letting it grow to the
    // freshly computed clamp(P99+3ms, 0, 200ms) if that rose (covering a
    // caller that polls without ever calling ReportLateEvent), then - once
    // a full kShrinkIntervalUs has elapsed since the last evaluation -
    // either taking one kShrinkStepUs shrink step toward the computed
    // value, or, if late events are still owed grace, consuming one
    // interval of that grace instead of shrinking.
    std::int64_t PlayoutTargetUs(std::int64_t nowUs);

    std::int64_t LateEventGraceOwed() const { return lateEventGraceOwed_; }

private:
    std::int64_t ComputeClampedTargetUs(std::int64_t nowUs) const;

    jamn::core::Histogram64 histogram_;
    std::int64_t targetUs_ = 0;
    std::int64_t minTargetUs_ = 0;
    std::int64_t lateEventGraceOwed_ = 0;
    std::int64_t lastEvalUs_ = 0;
    bool haveLastEval_ = false;
};

}  // namespace jamn::engine
