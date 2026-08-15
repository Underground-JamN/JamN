#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"
#include "jamn_proto/note_event.h"

namespace jamn::proto {

// Fixed capacity, not a wire limit - event_count is a full u8 (0-255) on
// the wire, but no realistic burst (even with Wave 4's K=3 redundancy,
// which re-includes the last 3 bursts' events) approaches that. A decoded
// event_count above this is rejected rather than causing an out-of-bounds
// write - exactly the shape tests/fuzz_corpus (T2.5) exists to exercise.
inline constexpr std::size_t kMaxEventsPerBurst = 64;

struct NoteBurst {
    std::int64_t baseTSessionUs = 0;
    std::uint16_t burstSeq = 0;
    std::uint8_t eventCount = 0;
    std::uint8_t reserved = 0;
    std::array<NoteEvent, kMaxEventsPerBurst> events{};

    static constexpr std::size_t kHeaderEncodedSize = 12;
};

bool EncodeNoteBurst(const NoteBurst& burst, jamn::core::ByteWriter& out);
bool DecodeNoteBurst(jamn::core::ByteReader& in, NoteBurst& out);

}  // namespace jamn::proto
