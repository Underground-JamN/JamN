#include "jamn_engine/audio_runtime.h"

#include <algorithm>

#include "jamn_core/time_types.h"

namespace jamn::engine {

void AudioRuntime::Prepare(double sampleRate, int blockFrames) {
    clock_.Prepare(sampleRate, blockFrames);

    // Service runs once per block, so a deadline can be a whole block
    // stale before anything looks at it. Telling the jitter buffers how
    // long a block is stops that being mistaken for network lateness -
    // without it the first remote note of every session is dropped.
    if (sampleRate > 0.0 && blockFrames > 0) {
        scheduler_.SetBlockPeriodUs(static_cast<std::int64_t>(1e6 * blockFrames / sampleRate));
    }
}

std::size_t AudioRuntime::Service(PeerRuntime& runtime, std::int64_t cumulativeSamples, std::int64_t steadyNs,
                                   Note* out, std::size_t capacity) {
    // One reading of "now" for the whole block. Taking it again partway
    // through would make two events in the same block resolve against
    // different presents, which is exactly the sort of thing that puts a
    // note on the wrong side of its deadline.
    const std::int64_t nowUs = steadyNs / 1000;

    clock_.Update(jamn::core::SampleTime(cumulativeSamples), steadyNs);

    NoteCrossing& crossing = runtime.crossing();

    // Which slots need an all-notes-off emitted this block, and for whom.
    // Collected here and emitted below rather than pushed through the
    // scheduler, because the scheduler could only tag it with a peer id -
    // and in the case that needs it most, the peer is already gone from
    // its slot, so a peer-to-slot lookup would find nothing and the
    // silence would never reach an instrument.
    std::array<jamn::net::PeerId, kMaxPeers> silence{};
    std::array<bool, kMaxPeers> needsSilence{};

    for (std::size_t slot = 0; slot < kMaxPeers; ++slot) {
        // Identity first: PeerRuntime's contract is that an offset read
        // from a slot whose peer has since changed means nothing, and this
        // is what says whether it changed.
        const jamn::net::PeerId peer = runtime.PeerAt(slot);
        const std::uint32_t generation = runtime.ReLockGeneration(slot);

        // A peer leaving costs exactly what a re-lock costs, and happens
        // far more often. Everything it had scheduled is meaningless now,
        // and among the discarded are the note-offs that would have ended
        // whatever it still has sounding - so without this its instrument
        // rings until something unrelated happens to stop it.
        if (lastPeer_[slot] != peer) {
            if (lastPeer_[slot] != PeerRuntime::kNoPeer) {
                stats_.notesFlushedOnPeerLoss += scheduler_.FlushPeer(lastPeer_[slot]);
                ++stats_.peersDeparted;
                silence[slot] = lastPeer_[slot];
                needsSilence[slot] = true;
            }
            lastPeer_[slot] = peer;
        }

        if (!haveReLockGen_[slot]) {
            // First sight of this slot is not a re-lock, whatever the
            // counter reads.
            lastReLockGen_[slot] = generation;
            haveReLockGen_[slot] = true;
        } else if (generation != lastReLockGen_[slot]) {
            lastReLockGen_[slot] = generation;
            ++stats_.reLocksSeen;
            if (peer != PeerRuntime::kNoPeer) {
                stats_.notesFlushedOnReLock += scheduler_.FlushPeer(peer);
                silence[slot] = peer;
                needsSilence[slot] = true;
            }
        }

        const std::int64_t offsetUs = runtime.PublishedOffsetUs(slot);
        const bool offsetLocked = runtime.OffsetIsLocked(slot);

        NoteCrossing::RemoteNote note;
        for (std::size_t drained = 0; drained < kMaxDrainPerPeerPerBlock; ++drained) {
            if (!crossing.Consume(slot, note)) break;

            if (peer == PeerRuntime::kNoPeer || note.peer != peer) {
                // The lane's tail outlived the link that filled it. Drop
                // rather than attribute it to whoever holds the slot now.
                ++stats_.notesFromStaleSlot;
                continue;
            }
            if (!offsetLocked) {
                ++stats_.notesBeforeClockLock;
            }

            // The one conversion this whole class exists around: the
            // sender's clock into ours. ClockSync publishes the offset as
            // remote-minus-local, so local time is the remote timestamp
            // less that offset. Every timestamp conversion happens here, on
            // the audio thread, and nowhere else (docs/CLOCK.md).
            const std::int64_t localUs = note.remoteSessionTimeUs - offsetUs;

            if (scheduler_.ScheduleRemoteEvent(peer, note.event, localUs, nowUs)) {
                ++stats_.notesScheduled;
            } else {
                ++stats_.notesRejectedByScheduler;
            }
        }
    }

    const std::size_t limit = std::min(capacity, kMaxNotesPerBlock);
    std::size_t count = 0;

    // Silences go out before any scheduled note, for two reasons. They must
    // not be crowded out by a full block - there are at most kMaxPeers of
    // them, so putting them first is what guarantees they fit - and a peer
    // that left and whose slot was re-claimed within one block would
    // otherwise silence the new occupant's first notes.
    for (std::size_t slot = 0; slot < kMaxPeers && count < limit; ++slot) {
        if (!needsSilence[slot]) continue;
        out[count].peer = silence[slot];
        out[count].slot = slot;
        out[count].event = jamn::proto::NoteEvent{};
        out[count].event.kind = jamn::proto::NoteEventKind::kAllNotesOff;
        out[count].sampleOffset = 0;
        ++count;
    }

    EventScheduler::Delivery delivery;
    // Bounded before the pop, never after: PopReady removes what it
    // returns, so discovering the array was full one note too late would
    // mean holding an event with nowhere to put it and no way back.
    while (count < limit && scheduler_.PopReady(nowUs, delivery)) {
        out[count].peer = delivery.peer;
        // PopReady hands back a PeerId, not a slot. kMaxPeers is 8 and a
        // block delivers a handful of notes, so a linear scan is cheaper
        // than the per-block reverse table it would take to avoid it -
        // and stays correct when a slot is re-claimed mid-block.
        out[count].slot = kMaxPeers;
        for (std::size_t slot = 0; slot < kMaxPeers; ++slot) {
            if (runtime.PeerAt(slot) == delivery.peer) {
                out[count].slot = slot;
                break;
            }
        }
        out[count].event = delivery.event;
        // Block granularity, deliberately. AudioClock could place this to
        // the sample, but IInstrument::NoteOn has nowhere to receive one -
        // see this field's declaration.
        out[count].sampleOffset = 0;
        ++count;
    }
    if (count == limit && limit > 0) {
        ++stats_.blocksAtNoteCapacity;
    }
    stats_.notesDelivered += count;

    return count;
}

}  // namespace jamn::engine
