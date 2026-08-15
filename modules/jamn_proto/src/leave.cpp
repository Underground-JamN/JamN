#include "jamn_proto/leave.h"

namespace jamn::proto {

bool EncodeLeave(const Leave& leave, jamn::core::ByteWriter& out) {
    return out.WriteU16(leave.peerId) && out.WriteU8(static_cast<std::uint8_t>(leave.reason));
}

bool DecodeLeave(jamn::core::ByteReader& in, Leave& out) {
    Leave leave;
    if (!in.ReadU16(leave.peerId)) return false;
    std::uint8_t reason = 0;
    if (!in.ReadU8(reason)) return false;
    switch (static_cast<LeaveReason>(reason)) {
        case LeaveReason::kVoluntary:
        case LeaveReason::kKicked:
        case LeaveReason::kTimedOut:
            leave.reason = static_cast<LeaveReason>(reason);
            break;
        default:
            leave.reason = LeaveReason::kVoluntary;  // See the header: tolerate, don't reject.
            break;
    }
    out = leave;
    return true;
}

}  // namespace jamn::proto
