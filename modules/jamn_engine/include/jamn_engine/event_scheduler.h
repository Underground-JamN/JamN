#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "jamn_core/session_limits.h"
#include "jamn_engine/deadline_resolver.h"
#include "jamn_engine/jitter_buffer.h"
#include "jamn_net/transport.h"
#include "jamn_proto/note_event.h"

namespace jamn::engine {

// One fixed-capacity min-heap keyed on deadline, plus a small fixed-
// capacity "held for state_rev" side list. Runs on the audio thread at
// block start (docs/CLOCK.md) - every operation here is allocation-free by
// construction: fixed arrays throughout, never a std::map or std::vector
// that could grow.
class EventScheduler {
public:
    static constexpr std::size_t kMaxScheduledEvents = 256;
    static constexpr std::size_t kMaxHeldEvents = 32;
    static constexpr std::size_t kMaxPeers = jamn::core::kMaxPeers;
    static constexpr std::int64_t kLateDropThresholdUs = 5000;
    static constexpr std::int64_t kStateRevHoldUs = 100'000;

    // Reserved peer_id for locally-originated input (this device's own
    // keyboard/mouse input) - never a real value a remote peer negotiates,
    // and used only to tag Delivery entries so a caller can tell local
    // input apart from a remote one after it comes out of PopReady.
    static constexpr jamn::net::PeerId kLocalPeerId = 0xFFFF;

    struct Delivery {
        jamn::net::PeerId peer = 0;
        jamn::proto::NoteEvent event;
    };

    EventScheduler() : resolver_(&defaultLiveResolver_) {}

    // Rejects (returns false, leaves the current resolver installed) any
    // resolver whose implemented() is false - the whole point of the
    // seam: the actual resolve path is provably unreachable, not merely
    // untested, for a resolver like MusicalResolver that has no TempoMap
    // to resolve against yet.
    bool SetResolver(IDeadlineResolver* resolver);

    void SetLocalRev(jamn::net::PeerId peer, std::uint16_t rev) { PeerSlot(peer).localRev = rev; }

    // Always scheduled at zero added delay, in either resolver mode - a
    // player's own local input is never delayed to align with anyone
    // else's timing (docs/CLOCK.md's "One scheduler, two resolvers", the
    // paragraph marked non-negotiable). Cited by section rather than by
    // line: the two line-numbered citations this file and jamn_app once
    // carried were both stale within days.
    bool ScheduleLocalEvent(const jamn::proto::NoteEvent& event, std::int64_t nowUs);

    // Applies the resolver + per-peer jitter-buffer deadline computation,
    // the late-event drop policy, and the state_rev hold. Returns false
    // only when the event was dropped outright (late and not a protected
    // kind) - note-offs and all-notes-off are never dropped, in any mode,
    // under any lateness.
    bool ScheduleRemoteEvent(jamn::net::PeerId peer, const jamn::proto::NoteEvent& event,
                              std::int64_t eventSessionTimeUs, std::int64_t nowUs);

    // Releases any held events whose wait is over, then pops and returns
    // the single earliest-deadline event whose deadline <= nowUs, if any.
    bool PopReady(std::int64_t nowUs, Delivery& out);

    // Discards everything queued or held for one peer, and returns how
    // many entries that was.
    //
    // Two things cost this: a ClockSync re-lock, after which every
    // deadline this peer has scheduled is wrong by however far the offset
    // moved; and the peer leaving, after which they mean nothing at all.
    //
    // **Discarding is only half of what either case needs.** Among the
    // discarded are the note-offs that would have ended whatever this peer
    // already has sounding, so a caller that stops here leaves those notes
    // ringing forever - and docs/CLOCK.md is explicit that a stuck note is
    // worse than a dropped one. Silencing is deliberately not done here:
    // this class knows a peer id, and the caller is the one that knows
    // which strip that peer's sound is coming out of. AudioRuntime pairs
    // the two.
    std::size_t FlushPeer(jamn::net::PeerId peer);

    // Everything queued or held, for every peer including the local one,
    // and returns how many entries that was. The panic path: a player
    // reaching for it wants silence now, and a queue that survives would
    // start sounding again on the next block. Silencing what is already
    // sounding is the caller's, exactly as it is for FlushPeer.
    std::size_t FlushAll();

    std::size_t ScheduledCount() const { return heapSize_; }
    std::size_t HeldCount() const { return heldCount_; }

    JitterBuffer& JitterBufferFor(jamn::net::PeerId peer) { return PeerSlot(peer).jitterBuffer; }

    // The audio block period, pushed down to every peer's jitter buffer as
    // a floor under its playout target - see JitterBuffer::SetMinTargetUs
    // for why a block-quantised consumer needs one. Applies to peers
    // already known and to any that appear later, so it does not matter
    // whether this is called before or after a peer joins.
    void SetBlockPeriodUs(std::int64_t blockPeriodUs);

private:
    struct HeapEntry {
        std::int64_t deadlineUs = 0;
        // Ties on deadline are broken by arrival order, and that is a
        // correctness requirement rather than tidiness - see HeapCompare.
        // 64 bits at a player's ~100 events/s is longer than the universe
        // has been running, so wrapping is not a case that needs handling.
        std::uint64_t sequence = 0;
        jamn::net::PeerId peer = 0;
        jamn::proto::NoteEvent event;
    };
    struct HeldEntry {
        jamn::net::PeerId peer = 0;
        jamn::proto::NoteEvent event;
        std::int64_t eventSessionTimeUs = 0;
        std::int64_t holdUntilUs = 0;
        bool inUse = false;
    };
    struct PeerState {
        jamn::net::PeerId peer = 0;
        JitterBuffer jitterBuffer;
        std::uint16_t localRev = 0;
        bool inUse = false;
    };

    static bool ProtectedFromDrop(const jamn::proto::NoteEvent& event) {
        return event.kind == jamn::proto::NoteEventKind::kNoteOff ||
               event.kind == jamn::proto::NoteEventKind::kAllNotesOff;
    }

    PeerState& PeerSlot(jamn::net::PeerId peer);

    // Computes the deadline/lateness policy and either pushes the event
    // onto the heap or drops it - the common path both ScheduleRemoteEvent
    // and a released held event go through.
    bool ResolveAndPush(jamn::net::PeerId peer, const jamn::proto::NoteEvent& event, std::int64_t eventSessionTimeUs,
                         std::int64_t nowUs);

    bool PushHeap(std::int64_t deadlineUs, jamn::net::PeerId peer, const jamn::proto::NoteEvent& event);
    void ReleaseDueHeldEvents(std::int64_t nowUs);
    // Min-heap on deadline, then on arrival order. **The tie-break is not
    // cosmetic.** Every local event scheduled in one block carries that
    // block's single `now` as its deadline, so a fast run across the
    // keys - mouse or, later, keyboard - puts a note's on and its own off
    // in the heap with identical deadlines. std::push_heap/pop_heap are
    // not stable, so ordering on deadline alone let an off be handed back
    // before its on, and a note that is switched off before it starts
    // sounds forever. Reported from a real drag, 2026-08-16.
    static bool HeapCompare(const HeapEntry& a, const HeapEntry& b) {
        if (a.deadlineUs != b.deadlineUs) return a.deadlineUs > b.deadlineUs;
        return a.sequence > b.sequence;
    }

    IDeadlineResolver* resolver_;
    LiveResolver defaultLiveResolver_;

    std::array<HeapEntry, kMaxScheduledEvents> heap_{};
    std::size_t heapSize_ = 0;
    // Never reset, including by FlushPeer: it orders entries against each
    // other, and restarting it while entries are still queued would make
    // a new arrival sort ahead of one already waiting.
    std::uint64_t nextSequence_ = 0;

    std::array<HeldEntry, kMaxHeldEvents> held_{};
    std::size_t heldCount_ = 0;

    std::array<PeerState, kMaxPeers> peers_{};
    std::size_t peerCount_ = 0;
    std::int64_t blockPeriodUs_ = 0;
};

}  // namespace jamn::engine
