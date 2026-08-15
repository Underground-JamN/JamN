#include "jamn_proto/negotiation.h"

#include <algorithm>

#include "jamn_proto/packet_header.h"

namespace jamn::proto {

NegotiationResult NegotiateVersion(std::uint8_t peerMajor, std::uint8_t peerMinor) {
    NegotiationResult result;
    if (peerMajor != kCurrentProtoMajor) {
        result.accepted = false;
        result.reason = "proto_major mismatch: local=" + std::to_string(static_cast<int>(kCurrentProtoMajor)) +
                         " peer=" + std::to_string(static_cast<int>(peerMajor));
        return result;
    }
    result.accepted = true;
    result.negotiatedMinor = std::min(kCurrentProtoMinor, peerMinor);
    return result;
}

}  // namespace jamn::proto
