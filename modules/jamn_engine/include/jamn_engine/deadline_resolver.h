#pragma once

#include <cstdint>

namespace jamn::engine {

// A runtime virtual, not a compile-time template parameter: Phase 2b needs
// a mid-session live/bar-synced mode swap at a bar boundary, which only a
// runtime seam supports. Runs on the audio thread at block start
// (docs/CLOCK.md), so docs/RT_RULES.md's no-exceptions-crossing-the-
// callback rule applies - implemented() exists precisely so "unimplemented"
// never has to mean throw.
class IDeadlineResolver {
public:
    virtual ~IDeadlineResolver() = default;

    // Whether ComputeDeadline is safe to call at all. A resolver returning
    // false here must never have ComputeDeadline invoked -
    // EventScheduler::SetResolver enforces that at install time, off the
    // audio thread, so the actual resolve path is provably unreachable
    // rather than merely untested.
    virtual bool implemented() const = 0;

    // Computes the absolute local deadline (microseconds, already
    // converted through ClockSync's offset by the caller) at which a
    // scheduled event should fire, given its own session-time timestamp
    // and the sending peer's current jitter-buffer playout delay.
    virtual std::int64_t ComputeDeadline(std::int64_t eventSessionTimeUs, std::int64_t playoutDelayUs) const = 0;
};

// deadline = t_session_us + jitterBuffer[peer].playoutDelay() - live from
// day one, per ARCHITECTURE_PLAN.md's Phase 0.5 scope line.
class LiveResolver : public IDeadlineResolver {
public:
    bool implemented() const override { return true; }
    std::int64_t ComputeDeadline(std::int64_t eventSessionTimeUs, std::int64_t playoutDelayUs) const override {
        return eventSessionTimeUs + playoutDelayUs;
    }
};

// Ships inert, with no TempoMap - TempoMap isn't due until Phase 2b, and
// this type's whole purpose in 0.5a is proving the one-scheduler-
// two-resolvers seam now rather than retrofitting it later
// (PHASE_0_5_PLAN.md's "Decisions already made"). implemented() is false,
// so EventScheduler::SetResolver refuses to install this; ComputeDeadline
// is consequently unreachable and exists only so this is a complete,
// linkable implementation of IDeadlineResolver.
class MusicalResolver : public IDeadlineResolver {
public:
    bool implemented() const override { return false; }
    std::int64_t ComputeDeadline(std::int64_t /*eventSessionTimeUs*/, std::int64_t /*playoutDelayUs*/) const override {
        return 0;
    }
};

}  // namespace jamn::engine
