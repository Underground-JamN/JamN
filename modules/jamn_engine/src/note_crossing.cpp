#include "jamn_engine/note_crossing.h"

namespace jamn::engine {

bool NoteCrossing::Publish(std::size_t slot, const RemoteNote& note) {
    if (slot >= kMaxPeers) return false;
    if (lanes_[slot].ring.Push(note)) return true;

    // Relaxed: this counter orders nothing and guards nothing. A reader
    // catching a stale value reports a slightly low drop count, which is
    // the whole consequence.
    lanes_[slot].dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool NoteCrossing::Consume(std::size_t slot, RemoteNote& out) {
    if (slot >= kMaxPeers) return false;
    return lanes_[slot].ring.Pop(out);
}

std::uint64_t NoteCrossing::DroppedCount(std::size_t slot) const {
    if (slot >= kMaxPeers) return 0;
    return lanes_[slot].dropped.load(std::memory_order_relaxed);
}

std::size_t NoteCrossing::DepthApprox(std::size_t slot) const {
    if (slot >= kMaxPeers) return 0;
    return lanes_[slot].ring.SizeApprox();
}

}  // namespace jamn::engine
