#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "jamn_core/session_limits.h"
#include "jamn_core/spsc_ring.h"
#include "jamn_net/transport.h"
#include "jamn_proto/note_event.h"

namespace jamn::engine {

// The one place a remote note crosses from the net thread to the audio
// thread. Every deduped event PeerRuntime accepts leaves through here and
// nowhere else, which is what makes "how does a received note reach the
// audio callback" a question with exactly one answer.
//
// **Single-producer by construction, not by convention.** ITransport::Poll
// fires every delivery synchronously on its caller, and PeerRuntime polls
// from one thread, so there is exactly one thread pushing - no argument
// about it is needed beyond that. The consumer is the audio thread, which
// drains at block start.
//
// One lane per peer slot rather than one shared ring, for two reasons that
// are not the SPSC argument (a single shared ring would also be SPSC, since
// there is one net thread and one audio thread either way):
//
//   - **Isolation.** A peer flooding its lane overflows only its own, and
//     drops only its own notes. On a shared ring, one misbehaving peer's
//     backlog would evict everyone else's notes.
//   - **Independent drain.** The audio thread already keeps per-peer
//     scheduler and jitter-buffer state, so it wants events grouped by
//     peer, not interleaved and re-sorted.
//
// This reuses `jamn::core::SpscRing`, the one sanctioned lock-free
// primitive - no new primitive, so no ADR (docs/RT_RULES.md).
class NoteCrossing {
public:
    static constexpr std::size_t kMaxPeers = jamn::core::kMaxPeers;

    // Roughly a second of backlog for a busy player (under 100 events/s)
    // against an audio thread that drains every block. Deep enough that
    // reaching the end means something is genuinely wrong, shallow enough
    // that all eight lanes together stay a few tens of kilobytes.
    static constexpr std::size_t kCapacityPerPeer = 128;

    // What crosses. Trivially copyable and owning no memory, because
    // SpscRing::Push copies and the pop side runs under the no-allocation
    // rule. `peer` is carried even though a lane already implies one: a
    // slot can be released and re-claimed by a different link while the
    // audio thread still has that link's tail to drain, and an item that
    // names its own peer cannot be misattributed when that happens.
    struct RemoteNote {
        jamn::net::PeerId peer = 0;
        // The *sender's* clock, unconverted. Turning it into a local time
        // needs this peer's published offset, and every timestamp
        // conversion belongs to the audio thread (docs/CLOCK.md).
        std::int64_t remoteSessionTimeUs = 0;
        jamn::proto::NoteEvent event;
    };

    // Net thread. False when the lane is full: the note is dropped and
    // counted, never blocked on and never buffered elsewhere. Backpressure
    // toward a real-time consumer is not an option, so a bounded drop is
    // the only honest failure.
    bool Publish(std::size_t slot, const RemoteNote& note);

    // Audio thread. Pops the oldest note for that slot, if any. Allocation-
    // free and lock-free, so it is safe to call from inside the callback.
    bool Consume(std::size_t slot, RemoteNote& out);

    // How many Publish calls this lane has refused for being full. That
    // equals notes lost only for a producer that does not retry, which the
    // runtime is - a caller that retries will see this climb without losing
    // anything. Readable from either thread; diagnostics only, never a
    // correctness input.
    std::uint64_t DroppedCount(std::size_t slot) const;

    // Approximate depth, for the same diagnostics-only purpose - it races a
    // concurrent Publish/Consume by design (SpscRing::SizeApprox).
    std::size_t DepthApprox(std::size_t slot) const;

private:
    struct Lane {
        jamn::core::SpscRing<RemoteNote, kCapacityPerPeer> ring;
        std::atomic<std::uint64_t> dropped{0};
    };

    std::array<Lane, kMaxPeers> lanes_;
};

static_assert(std::is_trivially_copyable_v<NoteCrossing::RemoteNote>,
              "RemoteNote crosses an SpscRing by copy and is popped on the audio thread - it must own no memory");

}  // namespace jamn::engine
