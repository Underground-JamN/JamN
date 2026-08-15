#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_proto/note_burst.h"
#include "jamn_proto/note_event.h"

namespace jamn::engine {

// K=3 redundancy: each outgoing burst re-includes every event queued in
// the previous 3 bursts alongside whatever was newly queued this cycle -
// so a live event that misses one burst still arrives redundantly in the
// next, and survives up to 2 consecutive burst losses in a row with zero
// added latency (it was already sent, just not yet received). With K=3 an
// event rides in 4 bursts total (1 original send + 3 redundant repeats).
// Lands in jamn_engine, feeding the scheduler directly, per the ownership
// decision (PHASE_0_5_PLAN.md).
class BurstAssembler {
public:
    static constexpr int kRedundancy = 3;
    static constexpr std::size_t kGenerations = kRedundancy + 1;

    // Queues one new local event for inclusion in the next burst built,
    // and (per K=3 redundancy) the kRedundancy bursts after that. Returns
    // false if this cycle's queue is already at jamn_proto::kMaxEventsPerBurst
    // capacity.
    //
    // eventTimeUs is the event's absolute time on this node's own session
    // clock, kept here rather than left to the caller because each of an
    // event's four copies rides a burst with a *different*
    // base_t_session_us: BuildNextBurst re-derives dt_us against whichever
    // base it is stamping, so every copy decodes to the same instant. Were
    // dt_us copied verbatim instead, a lost original would silently shift
    // the event later by one burst period per loss - the receiver dedupes
    // on (peer, event_seq), so whichever copy arrives first is the one
    // whose timestamp is used.
    bool QueueEvent(const jamn::proto::NoteEvent& event, std::int64_t eventTimeUs);

    // Builds the next outgoing burst: this cycle's newly queued events
    // plus every event still within the redundancy window from the
    // previous kRedundancy cycles, capped at
    // jamn_proto::kMaxEventsPerBurst total. Advances the assembler's
    // history by one generation - call this exactly once per burst
    // actually sent, since it consumes this cycle's queue.
    jamn::proto::NoteBurst BuildNextBurst(std::int64_t baseTSessionUs, std::uint16_t burstSeq);

private:
    struct Generation {
        std::array<jamn::proto::NoteEvent, jamn::proto::kMaxEventsPerBurst> events{};
        std::array<std::int64_t, jamn::proto::kMaxEventsPerBurst> times{};
        std::uint8_t count = 0;
    };

    // generations_[0] is always "queued this cycle, not yet sent";
    // generations_[1..kRedundancy] are the last kRedundancy sent cycles,
    // oldest last.
    std::array<Generation, kGenerations> generations_{};
};

}  // namespace jamn::engine
