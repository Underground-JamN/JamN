#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/session_limits.h"
#include "jamn_net/transport.h"

namespace jamn::engine {

// Receiver-side dedupe on (peer_id, event_seq) in a 1024-entry ring per
// peer. Feeds the EventScheduler directly, so it lives in jamn_engine
// rather than one layer down in jamn_proto (the wire-format module).
//
// Each ring slot stores the actual event_seq last recorded there (not just
// a bit), so a genuine repeat of that exact seq is caught by value
// equality regardless of the u16 wrap - the wrap only ever changes which
// *other* seq values alias onto the same slot, and those are distinguished
// by their stored value. A per-peer "highest seen" watermark, maintained
// with serial-number arithmetic (serial_compare.h) rather than a naive
// comparison, additionally rejects anything more than kRingSize behind the
// watermark outright - without that, a duplicate old enough that its slot
// has since been overwritten by something newer would be wrongly treated
// as fresh.
class DedupeRing {
public:
    static constexpr std::size_t kRingSize = 1024;
    static constexpr std::size_t kMaxPeers = jamn::core::kMaxPeers;

    // Returns true if (peer, seq) is a duplicate (already seen, or too far
    // behind the current watermark to trust) and should be discarded.
    // Returns false and marks it seen if this is genuinely new.
    bool IsDuplicate(jamn::net::PeerId peer, std::uint16_t seq);

private:
    struct Slot {
        std::uint16_t seq = 0;
        bool valid = false;
    };
    struct PeerRing {
        jamn::net::PeerId peer = 0;
        bool inUse = false;
        bool haveHighest = false;
        std::uint16_t highestSeq = 0;
        std::array<Slot, kRingSize> slots{};
    };

    PeerRing& RingFor(jamn::net::PeerId peer);

    std::array<PeerRing, kMaxPeers> rings_{};
    std::size_t peerCount_ = 0;
};

}  // namespace jamn::engine
