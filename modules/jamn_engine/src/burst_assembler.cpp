#include "jamn_engine/burst_assembler.h"

namespace jamn::engine {

bool BurstAssembler::QueueEvent(const jamn::proto::NoteEvent& event, std::int64_t eventTimeUs) {
    Generation& gen = generations_[0];
    if (gen.count >= jamn::proto::kMaxEventsPerBurst) return false;
    gen.events[gen.count] = event;
    gen.times[gen.count] = eventTimeUs;
    ++gen.count;
    return true;
}

jamn::proto::NoteBurst BurstAssembler::BuildNextBurst(std::int64_t baseTSessionUs, std::uint16_t burstSeq) {
    jamn::proto::NoteBurst burst;
    burst.baseTSessionUs = baseTSessionUs;
    burst.burstSeq = burstSeq;

    std::uint8_t total = 0;
    for (std::size_t g = 0; g < kGenerations; ++g) {
        for (std::uint8_t i = 0; i < generations_[g].count && total < jamn::proto::kMaxEventsPerBurst; ++i) {
            burst.events[total] = generations_[g].events[i];
            // Re-derived, never copied: the base moves with each burst, so
            // an event's four copies only decode to one instant if dt_us
            // moves with it. Bounded by kGenerations burst periods, i.e.
            // milliseconds - nowhere near an i32.
            burst.events[total].dtUs = static_cast<std::int32_t>(generations_[g].times[i] - baseTSessionUs);
            ++total;
        }
    }
    burst.eventCount = total;

    // Shift generations: the oldest (index kGenerations-1) falls out of
    // the redundancy window entirely; a fresh empty slot opens at index 0
    // for QueueEvent calls between now and the next BuildNextBurst.
    for (std::size_t g = kGenerations - 1; g > 0; --g) {
        generations_[g] = generations_[g - 1];
    }
    generations_[0] = Generation{};

    return burst;
}

}  // namespace jamn::engine
