#include "jamn_engine/dedupe_ring.h"

#include "jamn_engine/serial_compare.h"

namespace jamn::engine {

DedupeRing::PeerRing& DedupeRing::RingFor(jamn::net::PeerId peer) {
    for (std::size_t i = 0; i < peerCount_; ++i) {
        if (rings_[i].inUse && rings_[i].peer == peer) return rings_[i];
    }
    if (peerCount_ < kMaxPeers) {
        PeerRing& ring = rings_[peerCount_++];
        ring.peer = peer;
        ring.inUse = true;
        return ring;
    }
    // Over capacity - documented fallback, not exercised by any 0.5a
    // scenario (kMaxPeers comfortably covers the "2-6 friends" target).
    PeerRing& ring = rings_[kMaxPeers - 1];
    ring.peer = peer;
    return ring;
}

bool DedupeRing::IsDuplicate(jamn::net::PeerId peer, std::uint16_t seq) {
    PeerRing& ring = RingFor(peer);

    if (ring.haveHighest) {
        const auto distance = static_cast<std::int32_t>(static_cast<std::int16_t>(seq - ring.highestSeq));
        if (distance <= -static_cast<std::int32_t>(kRingSize)) {
            // More than a full ring behind the watermark - too old to
            // trust the ring's slot contents for; treat as a duplicate
            // without touching any state.
            return true;
        }
    }

    const std::size_t idx = seq % kRingSize;
    Slot& slot = ring.slots[idx];
    if (slot.valid && slot.seq == seq) {
        return true;  // Genuine repeat.
    }

    slot.valid = true;
    slot.seq = seq;
    if (!ring.haveHighest || IsNewerSerial(seq, ring.highestSeq)) {
        ring.highestSeq = seq;
        ring.haveHighest = true;
    }
    return false;
}

}  // namespace jamn::engine
