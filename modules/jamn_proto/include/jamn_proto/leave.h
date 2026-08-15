#pragma once

#include <cstddef>
#include <cstdint>

#include "jamn_core/byte_reader.h"
#include "jamn_core/byte_writer.h"

namespace jamn::proto {

// Why a peer left. Sent explicitly so the roster frees the slot the moment
// it is told to, rather than waiting for the transport's own disconnect
// detection - the two paths differ by seconds, and a slot held open that
// long is a slot the next joiner is refused from.
enum class LeaveReason : std::uint8_t {
    kVoluntary = 0,  // The peer chose to leave.
    kKicked = 1,     // The host removed it.
    kTimedOut = 2,   // No traffic within the session's tolerance.
};

// The leave message (MessageType::kLeave, channel 0 Control). peerId is the
// protocol-level peer_id of whoever is departing - not necessarily the
// sender, since the host announces a kicked or timed-out peer's departure
// to everyone else.
struct Leave {
    std::uint16_t peerId = 0;
    LeaveReason reason = LeaveReason::kVoluntary;

    static constexpr std::size_t kEncodedSize = 3;
};

bool EncodeLeave(const Leave& leave, jamn::core::ByteWriter& out);

// An unrecognised reason value decodes to kVoluntary rather than failing:
// per docs/PROTOCOL.md rule 2 a reader tolerates what a newer writer sent,
// and "someone left, cause unclear" is the safe reading - refusing to
// decode would leave the roster holding a slot for a peer that is gone.
bool DecodeLeave(jamn::core::ByteReader& in, Leave& out);

}  // namespace jamn::proto
