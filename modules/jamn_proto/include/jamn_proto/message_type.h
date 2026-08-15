#pragma once

#include <cstdint>

namespace jamn::proto {

// The TLV `type` field (docs/PROTOCOL.md's packet shape: `u16 type`). Every
// value here has a matching row in docs/PROTOCOL.md's message-type table -
// tools/check_protocol_doc.py enforces that the two never drift apart.
// "Built" below means a real message struct exists under jamn_proto today;
// "Reserved" means the value and its channel are locked so a later phase
// can add the struct without a proto_major bump (docs/PROTOCOL.md rule 5).
enum class MessageType : std::uint16_t {
    kJoin = 1,               // Built (Hello) - channel 0 Control.
    kLeave = 2,              // Built (Leave) - channel 0 Control.
    kSessionConfig = 3,      // Built (reserved SyncMode field) - channel 0 Control.
    kTempoMap = 4,           // Reserved - channel 0 Control.
    kTransport = 5,          // Reserved - channel 0 Control.
    kMixer = 6,              // Reserved - channel 0 Control.
    kInstrumentAssign = 7,   // Built (reserved soundfont triple) - channel 0 Control.
    kNoteStateDigest = 8,    // Reserved - channel 0 Control.
    kNoteBurst = 9,          // Built - channel 1 Realtime.
    kClockPing = 10,         // Built - channel 1 Realtime.
    kClockPong = 11,         // Built - channel 1 Realtime.
    kPeerStats = 12,         // Reserved - channel 1 Realtime.
};

}  // namespace jamn::proto
