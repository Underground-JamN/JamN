#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "jamn_core/session_limits.h"
#include "jamn_core/spsc_ring.h"
#include "jamn_engine/audio_clock.h"
#include "jamn_engine/event_scheduler.h"
#include "jamn_engine/peer_runtime.h"
#include "jamn_net/transport.h"
#include "jamn_proto/note_event.h"

namespace jamn::engine {

// The audio thread's owner, and the counterpart to PeerRuntime. One
// Service() call at block start does everything the audio thread owes the
// session: drive Clock 2, drain every peer's NoteCrossing lane, convert
// each note out of its sender's timebase, schedule it, and hand back
// whatever is due to sound this block.
//
// **It lives here, and it hands back data rather than making sound.**
// Turning a NoteEvent into audio needs jamn_dsp, and jamn_dsp and
// jamn_engine have no edge between them in either direction
// (docs/MODULE_OWNERSHIP.md). Rather than adding one, this stops at the
// last point that is pure timing: Service fills an array of Note and the
// caller - jamn_app, which already links both - walks it calling NoteOn
// and NoteOff. The split is the same one NetThread made for the same
// reason: everything with logic worth testing stays in a JUCE-free module
// that `ctest -L fast` and both sanitizer presets can see, and the
// JUCE-linked module keeps only what it cannot delegate. What is left
// there is a for-loop.
//
// **It also carries local input, and that is why Service takes a runtime
// that may be null.** A player's own notes are monitored live and never
// delayed (docs/CLOCK.md's "One scheduler, two resolvers", non-negotiable),
// so they do not travel through NoteCrossing, a jitter buffer, or any
// timestamp conversion - SubmitLocalNote hands them straight to
// ScheduleLocalEvent at zero added delay. That path owes nothing to a
// session, and a player running solo has no PeerRuntime at all, so gating
// it on one would make the app silent exactly when there is nobody else to
// hear.
//
// Threading: every method is the audio thread, except Prepare and
// SubmitLocalNote, which are the message thread. It reads PeerRuntime's
// per-slot atomics, which the net thread publishes and never expects back,
// and drains NoteCrossing, whose consumer side is this thread by contract.
class AudioRuntime {
public:
    static constexpr std::size_t kMaxPeers = jamn::core::kMaxPeers;

    // Ceiling on notes returned from one block. A player emits under 100
    // events/s, so eight peers at a 2.7ms block is far under one event per
    // block on average; this is sized for a burst arriving at once after a
    // stall, not for the steady state. Anything beyond it stays queued in
    // the scheduler and comes out next block rather than being dropped.
    static constexpr std::size_t kMaxNotesPerBlock = 64;

    // How many lane pops one block will do per peer. Bounds the work a
    // single block can be made to do by a peer that flooded its lane while
    // the audio thread was stalled - the rest waits, it is not discarded.
    static constexpr std::size_t kMaxDrainPerPeerPerBlock = 32;

    // The local-monitoring ring, sized on the same reasoning as
    // NoteCrossing's lanes: a player emits under 100 events/s against an
    // audio thread that drains every block, so reaching the end means the
    // audio thread stopped, not that somebody played fast.
    static constexpr std::size_t kLocalMonitorCapacity = 128;

    // The same bound as kMaxDrainPerPeerPerBlock, for the same reason: one
    // block's work stays bounded even if the ring filled during a stall.
    static constexpr std::size_t kMaxLocalDrainPerBlock = 32;

    struct Note {
        jamn::net::PeerId peer = 0;
        // The peer's slot, carried because it is what a caller actually
        // indexes with: PeerRuntime's slots and PeerMixer's strips are both
        // bounded by kMaxPeers and line up one to one, so a caller can
        // reach the right strip without searching a peer table. kMaxPeers
        // for the local peer, which owns no remote slot.
        std::size_t slot = kMaxPeers;
        jamn::proto::NoteEvent event;
        // Always 0 today: IInstrument::NoteOn takes no sample offset, so
        // there is nowhere to put a sub-block position even though
        // AudioClock can compute one. Present so that giving notes
        // sample-accurate placement later changes who fills this field
        // rather than changing this struct's shape. Consequence to be
        // honest about: placement is quantised to a block, 2.7ms at 128
        // frames and 48kHz.
        int sampleOffset = 0;
    };

    // Message thread, at device start - the audioDeviceAboutToStart
    // moment, not a callback. Re-preparing discards Clock 2's loop,
    // because a device restart is a new sample timeline.
    void Prepare(double sampleRate, int blockFrames);

    // Message thread, and the single producer of the local-monitoring
    // ring - keyboard capture and the mouse strip both funnel through one
    // message-thread entry point in jamn_app before reaching here, which
    // is what keeps this single-producer rather than needing a merge.
    // Returns false when the ring is full: the note is dropped and
    // counted, never blocked on, because backpressure toward a real-time
    // consumer is not an option.
    //
    // Reuses jamn::core::SpscRing, the one sanctioned lock-free primitive,
    // so no new primitive and no ADR (docs/RT_RULES.md) - the same
    // reasoning NoteCrossing's header states for the remote case.
    bool SubmitLocalNote(const jamn::proto::NoteEvent& event);

    // Message thread. Silence everything, at the next block: the queue is
    // discarded, whatever local input is still in the ring is thrown
    // away, and an all-notes-off is emitted for every peer slot and for
    // the local monitor. The escape hatch for a stuck note, whatever
    // caused it - a lost note-off from a peer, a window that lost focus
    // mid-chord, or a bug.
    //
    // Deliberately one-shot and not a mute: new notes arriving after it
    // play normally. A player reaching for panic wants the sound that is
    // happening to stop, not the session to end.
    //
    // Repeated calls before the next block collapse into one, which is
    // what a flag rather than a counter buys - two panics in 3ms are one
    // player hitting it twice, and there is nothing left to silence the
    // second time.
    void RequestPanic();

    // Audio thread, at block start. `runtime` may be null - a solo player
    // has no session, and local monitoring still has to sound. When it is
    // null nothing remote is drained or scheduled, and no per-slot state
    // is touched, so the first block after a session starts is not
    // mistaken for every peer departing at once. cumulativeSamples is the
    // index of this block's first frame and steadyNs is steady_clock at
    // callback entry - the pair JuceAudioDevice::RecordBlock accumulates.
    // Returns how many entries of `out` were filled.
    //
    // Allocation-free and lock-free throughout, so it is callable inside a
    // RealtimeScope. `capacity` should be at least kMaxPeers + 1: a panic
    // owes one all-notes-off per slot plus one for the local monitor, and
    // those are emitted first precisely so a smaller array cannot drop
    // them in favour of ordinary notes.
    std::size_t Service(PeerRuntime* runtime, std::int64_t cumulativeSamples, std::int64_t steadyNs, Note* out,
                        std::size_t capacity);

    const AudioClock& clock() const { return clock_; }
    EventScheduler& scheduler() { return scheduler_; }
    const EventScheduler& scheduler() const { return scheduler_; }

    // --- Diagnostics. Audio thread writes, and nothing reads them for a
    // correctness decision. ---

    struct Stats {
        std::uint64_t notesScheduled = 0;
        std::uint64_t notesDelivered = 0;
        // A note whose lane named a different peer than the slot does now:
        // the slot was released and re-claimed while its tail was still
        // undrained. Dropping it is correct - it belongs to a link that is
        // gone - and NoteCrossing::RemoteNote carries its own peer id
        // precisely so this is detectable rather than a misattribution.
        std::uint64_t notesFromStaleSlot = 0;
        // Arrived for a slot whose offset has never locked. Scheduled
        // anyway, against an assumed offset of zero, because refusing to
        // play anything until the clock locks would make the first seconds
        // of every session silent - but counted, because a session where
        // this keeps climbing is one where the clock never locked.
        std::uint64_t notesBeforeClockLock = 0;
        // The scheduler refused it: its queue is full, or it was late
        // enough to drop.
        std::uint64_t notesRejectedByScheduler = 0;
        std::uint64_t localNotesScheduled = 0;
        // A local note the scheduler refused, which for the local path can
        // only be a full heap - ScheduleLocalEvent takes no deadline
        // decision and so can never drop one for lateness. The most
        // user-visible drop there is: it is the player's own note not
        // sounding.
        std::uint64_t localNotesRejectedByScheduler = 0;
        std::uint64_t panicsServiced = 0;
        std::uint64_t notesFlushedOnPanic = 0;
        // Discarded by a re-lock flush, summed over every peer.
        std::uint64_t notesFlushedOnReLock = 0;
        std::uint64_t reLocksSeen = 0;
        // The same flush, for the far more common cause: a peer left, or
        // its slot was re-claimed by a different link.
        std::uint64_t notesFlushedOnPeerLoss = 0;
        std::uint64_t peersDeparted = 0;
        // Blocks that filled `capacity` and stopped popping. Whatever was
        // still due stays queued and comes out next block, one block late -
        // nothing is lost. Counted as blocks rather than notes because
        // finding out how many were left would mean popping them, which is
        // the thing being avoided.
        std::uint64_t blocksAtNoteCapacity = 0;
    };

    const Stats& stats() const { return stats_; }

    // How many SubmitLocalNote calls the ring has refused for being full.
    // Kept out of Stats because it is written by the message thread while
    // every other counter is the audio thread's - same split, and same
    // relaxed diagnostics-only contract, as NoteCrossing::DroppedCount.
    std::uint64_t LocalNotesDropped() const { return localNotesDropped_.load(std::memory_order_relaxed); }

private:
    AudioClock clock_;
    EventScheduler scheduler_;
    Stats stats_;

    // Message thread produces, audio thread consumes. Holds the event
    // alone: a local note needs no peer id (it is always kLocalPeerId) and
    // no timestamp (it is scheduled against the block's own `now`, which
    // is what "never delayed" means in practice).
    jamn::core::SpscRing<jamn::proto::NoteEvent, kLocalMonitorCapacity> localMonitor_;
    std::atomic<std::uint64_t> localNotesDropped_{0};
    // Set by the message thread, consumed by the audio thread at block
    // start. A flag rather than a ring: panic carries no data, and the
    // only question is whether one is owed.
    std::atomic<bool> panicRequested_{false};

    // Last re-lock generation seen per slot, so a change can be noticed.
    // Audio-thread-owned: PeerRuntime deliberately does nothing about a
    // re-lock except bump its counter, leaving scheduler state to whoever
    // owns the scheduler, which is this class.
    std::array<std::uint32_t, kMaxPeers> lastReLockGen_{};
    // Whether lastReLockGen_ has ever been read for that slot. Without it
    // the first block of a session looks like a re-lock on every slot
    // whose counter is not zero.
    std::array<bool, kMaxPeers> haveReLockGen_{};
    // Which peer was in each slot last block, so a departure can be
    // noticed. Starts as "nobody", which is what an unoccupied slot reads
    // as - so a slot's first peer arriving is not mistaken for a
    // departure.
    std::array<jamn::net::PeerId, kMaxPeers> lastPeer_ = [] {
        std::array<jamn::net::PeerId, kMaxPeers> initial{};
        initial.fill(PeerRuntime::kNoPeer);
        return initial;
    }();
};

}  // namespace jamn::engine
