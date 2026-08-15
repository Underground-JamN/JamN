#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "jamn_core/byte_reader.h"
#include "jamn_core/session_limits.h"
#include "jamn_core/spsc_ring.h"
#include "jamn_engine/burst_assembler.h"
#include "jamn_engine/clock_sync.h"
#include "jamn_engine/dedupe_ring.h"
#include "jamn_engine/note_crossing.h"
#include "jamn_net/transport.h"
#include "jamn_proto/message_type.h"
#include "jamn_proto/note_event.h"

namespace jamn::engine {

// The production owner of the net thread: the transport poll loop, the
// ClockSync ping cadence and its pong replies, burst assembly and send, and
// receive -> dedupe -> hand off. Before this existed, the only assembly of
// those pieces was test code, which meant the acceptance suite was proving
// its own reimplementation rather than the thing that ships.
//
// **It owns the net thread only.** JitterBuffer and EventScheduler stay
// owned by the audio thread, and every conversion of a remote SessionTime
// into a local one happens there too (docs/CLOCK.md) - which is why the
// note-delivery callback below reports the *remote* session time untouched
// rather than a converted one. Nothing here reads or writes scheduler
// state.
//
// Session-agnostic on purpose. It never links jamn_session: control-channel
// packets are handed straight out through SetControlPacketCallback for
// whoever owns the session layer (SessionHost) to decode, which is exactly
// the division session_host.h already describes from its side - the runtime
// stays the single owner of the transport's callbacks, and the host is
// driven by it rather than competing for them.
//
// Threading, stated once because everything below depends on it:
//
//   - Service() and everything it reaches is the **net thread**. That is
//     one thread by construction, not by convention: ITransport::Poll fires
//     every receive and peer-event callback synchronously on its caller.
//   - SubmitLocalEvent() is the **message thread**, and is the only method
//     safe to call from anywhere but the net thread. It is the single
//     producer of an SpscRing the net thread drains.
//   - crossing() and PeerAt/PublishedOffsetUs/ReLockGeneration are for the
//     **audio thread**: one SpscRing lane plus three atomics per peer slot,
//     published by the net thread and never mutated by the reader. All of
//     them are indexed by *slot*, not by PeerId, precisely so the audio
//     thread never has to walk a peer table the net thread is concurrently
//     mutating.
class PeerRuntime {
public:
    static constexpr std::size_t kMaxPeers = jamn::core::kMaxPeers;

    // 3ms, the flush cadence the K=3 redundancy window is sized against:
    // an event rides in 4 bursts, so it survives 2 consecutive losses
    // within ~12ms and with zero added latency (docs/PROTOCOL.md).
    static constexpr std::int64_t kDefaultBurstPeriodUs = 3000;

    // One burst period's worth of local input, with room to spare - a busy
    // player emits under 100 events/s, i.e. well under one event per 3ms
    // burst. A full ring drops rather than blocks; SubmitLocalEvent says so.
    static constexpr std::size_t kLocalEventCapacity = 128;

    static constexpr std::size_t kPacketBufferBytes = 1500;

    // ENET_HOST_DEFAULT_MTU (1392) less ENet's own protocol overhead. A
    // kRealtime payload above the live peer's fragment threshold does not
    // become unreliable-fragmented - it silently becomes reliable, ordered
    // and head-of-line blocking (enet_transport.h explains why) - so the
    // budget is enforced here rather than discovered as a latency bug.
    // T4.3 narrows this to the negotiated per-peer value.
    static constexpr std::size_t kDefaultMaxRealtimePayload = 1364;

    // A slot with no peer in it. 0xFFFF is already reserved as
    // EventScheduler::kLocalPeerId and so is never a real remote link.
    static constexpr jamn::net::PeerId kNoPeer = 0xFFFF;

    // Counters, not correctness state - a test asserts on them and Wave 6's
    // diagnostics panel will want them. Net thread only.
    struct Stats {
        std::uint64_t burstsSent = 0;
        std::uint64_t packetsTooLarge = 0;
        std::uint64_t sendsFailed = 0;
        std::uint64_t packetsReceived = 0;
        std::uint64_t packetsRejected = 0;
        std::uint64_t packetsFromUnknownPeer = 0;
        std::uint64_t notesDelivered = 0;
        std::uint64_t notesDeduped = 0;
        std::uint64_t notesDroppedAtCrossing = 0;
        std::uint64_t localEventsDropped = 0;
        std::uint64_t pingsSent = 0;
        std::uint64_t pongsSent = 0;
        std::uint64_t clockSamplesFolded = 0;
    };

    // One whole control-channel packet, undecoded - header included, cursor
    // at the start. This is SessionHost::HandleControlPacket's exact
    // signature, so wiring the two together is one line and neither module
    // gains an edge on the other.
    using ControlPacketCallback = std::function<void(jamn::net::PeerId from, jamn::core::ByteReader& packet)>;

    // Forwarded after the runtime has claimed or released the peer's own
    // slot, so a handler that consults PeerAt() sees the settled table.
    using PeerEventCallback = std::function<void(jamn::net::PeerId peer, jamn::net::PeerEvent event)>;

    // sessionStartUs anchors ClockSync's 4Hz-for-5s-then-1Hz ping cadence;
    // it is the caller's monotonic clock, the same one Service() is passed.
    explicit PeerRuntime(jamn::net::ITransport& transport, std::int64_t sessionStartUs = 0);
    ~PeerRuntime();

    PeerRuntime(const PeerRuntime&) = delete;
    PeerRuntime& operator=(const PeerRuntime&) = delete;

    // Where every deduped remote note leaves this object, and the only
    // place it does. Published by the net thread lane by lane; the audio
    // thread drains it at block start. There is deliberately no net-thread
    // delivery callback beside it - a second exit would be a second path to
    // keep correct, and the one that ships would be the one tests skip.
    NoteCrossing& crossing() { return crossing_; }
    const NoteCrossing& crossing() const { return crossing_; }

    void SetControlPacketCallback(ControlPacketCallback callback) { controlCallback_ = std::move(callback); }
    void SetPeerEventCallback(PeerEventCallback callback) { peerEventCallback_ = std::move(callback); }

    // The protocol-level peer_id this node stamps into every outgoing
    // PacketHeader. Zero until a host assigns one, which is why
    // PacketHeader's own default is zero too - an unassigned id must not
    // collide with an assigned one.
    void SetLocalPeerId(std::uint16_t peerId) { localPeerId_ = peerId; }

    void SetBurstPeriodUs(std::int64_t periodUs) { burstPeriodUs_ = periodUs; }
    void SetMaxRealtimePayload(std::size_t bytes) { maxRealtimePayload_ = bytes; }

    // Claims a slot for a link, idempotent for a link already present.
    // False only when every slot is taken - the caller refuses the link
    // rather than the runtime overflowing. Normally driven by the
    // transport's own kConnected event; exposed for a caller that has no
    // peer-event path (a fixed-topology harness, say).
    bool AddPeer(jamn::net::PeerId peer);

    // Releases the slot and forgets the peer's clock estimate outright. A
    // peer that reconnects gets a fresh ClockSync, not a stale lock.
    void RemovePeer(jamn::net::PeerId peer);

    // Queues one locally-originated event for transmission. **The only
    // method callable off the net thread**, and callable from exactly one
    // other thread - it is the single producer of an SpscRing. Returns
    // false, dropping the event, if that ring is full; it never blocks and
    // never allocates.
    //
    // event.eventSeq is ignored and overwritten: the runtime is the single
    // source of this node's sequence numbers, because the receiver's dedupe
    // ring keys on them and two producers would alias.
    //
    // This is the transmit half only. Zero-delay local monitoring is a
    // second, separate ring to the audio thread (docs/CLOCK.md: a player's
    // own input is never delayed) - one producer feeding two consumers is
    // two SPSC rings, never one ring with two readers.
    bool SubmitLocalEvent(const jamn::proto::NoteEvent& event, std::int64_t eventTimeUs);

    // One net-thread service cycle: poll the transport (which is what fires
    // every receive and peer-event callback), then drain local input, send
    // any due clock ping, and build and broadcast the burst if one is due.
    // Ingest before produce, so an event received this cycle can influence
    // what this cycle sends.
    void Service(std::int64_t nowUs);

    // --- Audio thread reads only, by slot index. ---

    // Which link occupies this slot, or kNoPeer. Read this first: an offset
    // read from a slot whose peer has since changed is meaningless, and the
    // link identity is what says so.
    jamn::net::PeerId PeerAt(std::size_t slot) const;

    // The peer's ClockSync offset estimate as of the last folded sample.
    // Zero for a slot that has never locked - and zero is *also* a
    // perfectly legitimate estimate (two peers on one machine share a
    // clock), so read OffsetIsLocked before trusting it. The value alone
    // cannot distinguish the two.
    std::int64_t PublishedOffsetUs(std::size_t slot) const;

    // Whether the offset above comes from a locked estimate rather than
    // from a slot that has never had one. Published alongside it for the
    // reason just given: without this, "no correction known yet" and "the
    // correction is zero" are the same reading.
    bool OffsetIsLocked(std::size_t slot) const;

    // Bumped once per ClockSync re-lock. The audio thread compares it
    // against its own last-seen value each block and, on a change, flushes
    // every note it is holding for that peer - a stuck note is worse than a
    // dropped one (docs/CLOCK.md). The re-lock itself fires synchronously
    // inside AddSample on the net thread, so this counter is deliberately
    // all the net thread does about it: scheduler state stays audio-owned.
    std::uint32_t ReLockGeneration(std::size_t slot) const;

    // --- Net thread. ---

    const Stats& stats() const { return stats_; }
    std::size_t PeerCount() const;
    const ClockSync* ClockSyncFor(jamn::net::PeerId peer) const;

private:
    struct PeerSlot {
        jamn::net::PeerId link = kNoPeer;
        bool inUse = false;
        ClockSync clockSync;
        std::int64_t lastPingUs = 0;
        bool havePinged = false;
    };

    // What crosses the local-input ring. A plain aggregate of two trivially
    // copyable members, because SpscRing::Push copies and must never queue
    // anything owning memory.
    struct LocalEvent {
        jamn::proto::NoteEvent event;
        std::int64_t timeUs = 0;
    };

    void OnReceive(jamn::net::PeerId from, jamn::net::Channel channel, jamn::core::ByteReader& packet);
    void OnPeerEvent(jamn::net::PeerId peer, jamn::net::PeerEvent event);

    void HandleRealtimePacket(jamn::net::PeerId from, std::size_t slot, jamn::core::ByteReader& packet);
    void HandleNoteBurst(jamn::net::PeerId from, std::size_t slot, jamn::core::ByteReader& value);
    void HandleClockPing(jamn::net::PeerId from, jamn::core::ByteReader& value);
    void HandleClockPong(std::size_t slot, jamn::core::ByteReader& value);

    void DrainLocalEvents();
    void SendDueClockPings();
    void SendDueBurst();

    // Frames value into packetBuffer_ as one PacketHeader + one TLV, and
    // reports the framed size. False if it would not fit the buffer or the
    // realtime payload budget.
    bool FramePacket(jamn::proto::MessageType type, const std::uint8_t* value, std::size_t len, std::size_t& outSize);

    void SendFramed(jamn::net::PeerId peer, jamn::net::Channel channel, std::size_t size);
    void BroadcastFramed(jamn::net::Channel channel, std::size_t size);

    // Slot lookup is linear over a fixed array of 8 and always walks in
    // slot order - both because kMaxPeers is tiny and because a fixed
    // iteration order is what keeps a seeded sim byte-identical run to run.
    std::size_t FindSlot(jamn::net::PeerId peer) const;
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    jamn::net::ITransport& transport_;
    std::int64_t sessionStartUs_ = 0;

    // The nowUs of the Service call currently in progress. Every callback
    // this class installs fires inside that call, so this is the one clock
    // reading they all share - a pong's t2/t3 and a folded sample's t4 come
    // from here rather than from a second, slightly different reading.
    std::int64_t serviceNowUs_ = 0;

    std::uint16_t localPeerId_ = 0;
    std::int64_t burstPeriodUs_ = kDefaultBurstPeriodUs;
    std::size_t maxRealtimePayload_ = kDefaultMaxRealtimePayload;

    std::int64_t lastBurstUs_ = 0;
    bool haveSentBurst_ = false;
    std::uint16_t nextBurstSeq_ = 0;
    std::uint16_t nextEventSeq_ = 0;
    std::uint16_t nextPingSeq_ = 0;

    BurstAssembler assembler_;
    DedupeRing dedupe_;
    NoteCrossing crossing_;
    jamn::core::SpscRing<LocalEvent, kLocalEventCapacity> localEvents_;

    std::array<PeerSlot, kMaxPeers> slots_{};

    // Published by the net thread, read by the audio thread. Separate from
    // slots_ rather than atomics inside it, so the audio thread touches
    // exactly these three arrays and nothing else in this object.
    std::array<std::atomic<jamn::net::PeerId>, kMaxPeers> slotPeer_{};
    std::array<std::atomic<std::int64_t>, kMaxPeers> slotOffsetUs_{};
    std::array<std::atomic<bool>, kMaxPeers> slotOffsetLocked_{};
    std::array<std::atomic<std::uint32_t>, kMaxPeers> slotReLockGen_{};

    std::array<std::uint8_t, kPacketBufferBytes> packetBuffer_{};
    std::array<std::uint8_t, kPacketBufferBytes> valueBuffer_{};

    Stats stats_;

    ControlPacketCallback controlCallback_;
    PeerEventCallback peerEventCallback_;
};

}  // namespace jamn::engine
