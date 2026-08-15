#include "jamn_engine/event_scheduler.h"

#include <algorithm>

namespace jamn::engine {

EventScheduler::PeerState& EventScheduler::PeerSlot(jamn::net::PeerId peer) {
    for (std::size_t i = 0; i < peerCount_; ++i) {
        if (peers_[i].inUse && peers_[i].peer == peer) return peers_[i];
    }
    if (peerCount_ < kMaxPeers) {
        PeerState& slot = peers_[peerCount_++];
        slot.peer = peer;
        slot.inUse = true;
        slot.jitterBuffer.SetMinTargetUs(blockPeriodUs_);
        return slot;
    }
    // Over capacity - reuse the last slot as a documented fallback, not
    // exercised by any 0.5a scenario (kMaxPeers comfortably covers the
    // "2-6 friends" target this app is built for).
    PeerState& slot = peers_[kMaxPeers - 1];
    slot.peer = peer;
    return slot;
}

void EventScheduler::SetBlockPeriodUs(std::int64_t blockPeriodUs) {
    blockPeriodUs_ = blockPeriodUs;
    for (std::size_t i = 0; i < peerCount_; ++i) peers_[i].jitterBuffer.SetMinTargetUs(blockPeriodUs);
}

bool EventScheduler::SetResolver(IDeadlineResolver* resolver) {
    if (resolver == nullptr || !resolver->implemented()) return false;
    resolver_ = resolver;
    return true;
}

bool EventScheduler::PushHeap(std::int64_t deadlineUs, jamn::net::PeerId peer, const jamn::proto::NoteEvent& event) {
    if (heapSize_ >= kMaxScheduledEvents) return false;
    heap_[heapSize_] = HeapEntry{deadlineUs, peer, event};
    ++heapSize_;
    std::push_heap(heap_.begin(), heap_.begin() + heapSize_, HeapCompare);
    return true;
}

bool EventScheduler::ScheduleLocalEvent(const jamn::proto::NoteEvent& event, std::int64_t nowUs) {
    return PushHeap(nowUs, kLocalPeerId, event);
}

bool EventScheduler::ResolveAndPush(jamn::net::PeerId peer, const jamn::proto::NoteEvent& event,
                                     std::int64_t eventSessionTimeUs, std::int64_t nowUs) {
    PeerState& ps = PeerSlot(peer);
    // Playout delay is computed from history that predates this event -
    // its own transit time is recorded afterward, below, so it feeds
    // future decisions rather than retroactively excusing its own
    // lateness.
    const std::int64_t playoutDelay = ps.jitterBuffer.PlayoutTargetUs(nowUs);
    const std::int64_t deadline = resolver_->ComputeDeadline(eventSessionTimeUs, playoutDelay);
    const std::int64_t latenessUs = nowUs - deadline;
    const std::int64_t observedTransitUs = nowUs - eventSessionTimeUs;
    const bool protectedEvent = ProtectedFromDrop(event);

    if (latenessUs >= kLateDropThresholdUs) {
        ps.jitterBuffer.RecordTransit(observedTransitUs, nowUs);
        ps.jitterBuffer.ReportLateEvent(nowUs);
        if (!protectedEvent) return false;  // Dropped.
        // Note-offs and all-notes-off are never dropped, however late.
        return PushHeap(nowUs, peer, event);
    }

    ps.jitterBuffer.RecordTransit(observedTransitUs, nowUs);

    if (latenessUs > 0) {
        // Late but under the drop threshold - plays at sample 0 of the
        // current block rather than at a deadline already in the past.
        return PushHeap(nowUs, peer, event);
    }

    return PushHeap(deadline, peer, event);
}

bool EventScheduler::ScheduleRemoteEvent(jamn::net::PeerId peer, const jamn::proto::NoteEvent& event,
                                          std::int64_t eventSessionTimeUs, std::int64_t nowUs) {
    PeerState& ps = PeerSlot(peer);
    if (event.stateRev > ps.localRev) {
        // A note can arrive before the parameters it was played under
        // (channels 0 and 1 have no ordering relative to each other) -
        // hold up to kStateRevHoldUs rather than stalling forever.
        for (auto& h : held_) {
            if (!h.inUse) {
                h.inUse = true;
                h.peer = peer;
                h.event = event;
                h.eventSessionTimeUs = eventSessionTimeUs;
                h.holdUntilUs = nowUs + kStateRevHoldUs;
                ++heldCount_;
                return true;  // Accepted (held), not dropped.
            }
        }
        // Held list is full - schedule immediately rather than silently
        // discarding a real event; a full held-list is a documented
        // capacity limit, not a reason to drop.
    }
    return ResolveAndPush(peer, event, eventSessionTimeUs, nowUs);
}

void EventScheduler::ReleaseDueHeldEvents(std::int64_t nowUs) {
    for (auto& h : held_) {
        if (!h.inUse) continue;
        PeerState& ps = PeerSlot(h.peer);
        const bool revReady = h.event.stateRev <= ps.localRev;
        const bool timedOut = nowUs >= h.holdUntilUs;
        if (!revReady && !timedOut) continue;

        h.inUse = false;
        --heldCount_;
        // Either way, play it now rather than re-running the late-drop
        // policy: the state_rev hold is a deliberate, protocol-driven
        // delay we imposed, not network jitter, so re-evaluating
        // "lateness" against the original arrival time here would punish
        // an event for exactly the wait this mechanism exists to grant it
        // - up to and including dropping a plain note-on that timed out,
        // which would defeat "plays it anyway rather than stalling
        // forever."
        PushHeap(nowUs, h.peer, h.event);
    }
}

std::size_t EventScheduler::FlushPeer(jamn::net::PeerId peer) {
    std::size_t discarded = 0;

    // Compact the heap's backing array in place, then re-heapify once.
    // Popping matching entries one at a time would be O(n log n) restores
    // for what is a single bulk removal, and this runs on the audio thread.
    std::size_t kept = 0;
    for (std::size_t index = 0; index < heapSize_; ++index) {
        if (heap_[index].peer == peer) {
            ++discarded;
            continue;
        }
        heap_[kept] = heap_[index];
        ++kept;
    }
    if (kept != heapSize_) {
        heapSize_ = kept;
        std::make_heap(heap_.begin(), heap_.begin() + heapSize_, HeapCompare);
    }

    for (auto& h : held_) {
        if (!h.inUse || h.peer != peer) continue;
        h.inUse = false;
        --heldCount_;
        ++discarded;
    }

    return discarded;
}

bool EventScheduler::PopReady(std::int64_t nowUs, Delivery& out) {
    ReleaseDueHeldEvents(nowUs);
    if (heapSize_ == 0) return false;
    if (heap_[0].deadlineUs > nowUs) return false;

    out.peer = heap_[0].peer;
    out.event = heap_[0].event;
    std::pop_heap(heap_.begin(), heap_.begin() + heapSize_, HeapCompare);
    --heapSize_;
    return true;
}

}  // namespace jamn::engine
