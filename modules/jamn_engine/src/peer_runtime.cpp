#include "jamn_engine/peer_runtime.h"

#include "jamn_core/byte_writer.h"
#include "jamn_proto/clock_pingpong.h"
#include "jamn_proto/note_burst.h"
#include "jamn_proto/packet_header.h"
#include "jamn_proto/tlv.h"

namespace jamn::engine {

using jamn::core::ByteReader;
using jamn::core::ByteWriter;
using jamn::net::Channel;
using jamn::net::PeerEvent;
using jamn::net::PeerId;
using jamn::proto::MessageType;

PeerRuntime::PeerRuntime(jamn::net::ITransport& transport, std::int64_t sessionStartUs)
    : transport_(transport), sessionStartUs_(sessionStartUs), serviceNowUs_(sessionStartUs) {
    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        slotPeer_[i].store(kNoPeer, std::memory_order_relaxed);
        slotOffsetUs_[i].store(0, std::memory_order_relaxed);
        slotOffsetLocked_[i].store(false, std::memory_order_relaxed);
        slotReLockGen_[i].store(0, std::memory_order_relaxed);
    }

    transport_.SetReceiveCallback(
        [this](PeerId from, Channel channel, ByteReader& body) { OnReceive(from, channel, body); });
    transport_.SetPeerEventCallback([this](PeerId peer, PeerEvent event) { OnPeerEvent(peer, event); });
}

PeerRuntime::~PeerRuntime() {
    // The callbacks installed above captured `this`. A transport outliving
    // its runtime and still holding them would call into freed memory on
    // its next Poll, so they are cleared here rather than left to the
    // caller to remember.
    transport_.SetReceiveCallback(nullptr);
    transport_.SetPeerEventCallback(nullptr);
}

std::size_t PeerRuntime::FindSlot(PeerId peer) const {
    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        if (slots_[i].inUse && slots_[i].link == peer) return i;
    }
    return kNoSlot;
}

bool PeerRuntime::AddPeer(PeerId peer) {
    if (peer == kNoPeer) return false;
    if (FindSlot(peer) != kNoSlot) return true;

    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        if (slots_[i].inUse) continue;

        slots_[i] = PeerSlot{};
        slots_[i].link = peer;
        slots_[i].inUse = true;
        // The re-lock fires synchronously inside AddSample, on this thread.
        // All it may do is bump a counter: "flush every held note" is
        // audio-thread state, and reaching into EventScheduler from here
        // would be exactly the cross-thread mutation the counter exists to
        // avoid.
        slots_[i].clockSync.SetReLockCallback(
            [this, i] { slotReLockGen_[i].fetch_add(1, std::memory_order_release); });

        slotOffsetUs_[i].store(0, std::memory_order_relaxed);
        slotOffsetLocked_[i].store(false, std::memory_order_relaxed);
        // Published last, with release: it is what says the others are
        // this peer's, so the audio thread must not see it before them.
        slotPeer_[i].store(peer, std::memory_order_release);
        return true;
    }
    return false;
}

void PeerRuntime::RemovePeer(PeerId peer) {
    const std::size_t slot = FindSlot(peer);
    if (slot == kNoSlot) return;

    // Retired before the state it describes is torn down, so the audio
    // thread never reads an offset belonging to a peer that has gone.
    slotPeer_[slot].store(kNoPeer, std::memory_order_release);
    slots_[slot] = PeerSlot{};
    slotOffsetUs_[slot].store(0, std::memory_order_relaxed);
    slotOffsetLocked_[slot].store(false, std::memory_order_relaxed);
}

std::size_t PeerRuntime::PeerCount() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        if (slots_[i].inUse) ++count;
    }
    return count;
}

const ClockSync* PeerRuntime::ClockSyncFor(PeerId peer) const {
    const std::size_t slot = FindSlot(peer);
    return slot == kNoSlot ? nullptr : &slots_[slot].clockSync;
}

PeerId PeerRuntime::PeerAt(std::size_t slot) const {
    if (slot >= kMaxPeers) return kNoPeer;
    return slotPeer_[slot].load(std::memory_order_acquire);
}

std::int64_t PeerRuntime::PublishedOffsetUs(std::size_t slot) const {
    if (slot >= kMaxPeers) return 0;
    return slotOffsetUs_[slot].load(std::memory_order_acquire);
}

bool PeerRuntime::OffsetIsLocked(std::size_t slot) const {
    if (slot >= kMaxPeers) return false;
    return slotOffsetLocked_[slot].load(std::memory_order_acquire);
}

std::uint32_t PeerRuntime::ReLockGeneration(std::size_t slot) const {
    if (slot >= kMaxPeers) return 0;
    return slotReLockGen_[slot].load(std::memory_order_acquire);
}

bool PeerRuntime::SubmitLocalEvent(const jamn::proto::NoteEvent& event, std::int64_t eventTimeUs) {
    LocalEvent local;
    local.event = event;
    local.timeUs = eventTimeUs;
    return localEvents_.Push(local);
}

void PeerRuntime::Service(std::int64_t nowUs) {
    serviceNowUs_ = nowUs;

    // Ingest first: every receive and peer-event callback fires inside this
    // call, on this thread, so an event that arrives now can still affect
    // what the produce half below sends.
    transport_.Poll(nowUs);

    DrainLocalEvents();
    SendDueClockPings();
    SendDueBurst();
}

void PeerRuntime::DrainLocalEvents() {
    LocalEvent local;
    while (localEvents_.Pop(local)) {
        // Stamped here and nowhere else. The receiver's dedupe ring keys on
        // (peer, event_seq), so two sources of this number would alias into
        // silently dropped notes.
        local.event.eventSeq = nextEventSeq_++;
        if (!assembler_.QueueEvent(local.event, local.timeUs)) {
            // This cycle's queue is full. A gap in the sequence is harmless
            // - the dedupe watermark is serial-number arithmetic, not a
            // contiguity check.
            ++stats_.localEventsDropped;
        }
    }
}

void PeerRuntime::SendDueClockPings() {
    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        PeerSlot& slot = slots_[i];
        if (!slot.inUse) continue;
        // A peer that has never been pinged is due immediately: ShouldPing
        // measures an interval since the last ping, and there isn't one.
        if (slot.havePinged && !ClockSync::ShouldPing(serviceNowUs_, sessionStartUs_, slot.lastPingUs)) continue;

        jamn::proto::ClockPing ping;
        ping.pingSeq = nextPingSeq_++;
        ping.t1 = serviceNowUs_;

        ByteWriter value(valueBuffer_.data(), valueBuffer_.size());
        if (!jamn::proto::EncodeClockPing(ping, value)) continue;
        std::size_t size = 0;
        if (!FramePacket(MessageType::kClockPing, valueBuffer_.data(), value.Position(), size)) continue;

        SendFramed(slot.link, Channel::kRealtime, size);
        slot.lastPingUs = serviceNowUs_;
        slot.havePinged = true;
        ++stats_.pingsSent;
    }
}

void PeerRuntime::SendDueBurst() {
    if (haveSentBurst_ && serviceNowUs_ - lastBurstUs_ < burstPeriodUs_) return;
    lastBurstUs_ = serviceNowUs_;
    haveSentBurst_ = true;

    // Built every period whether or not anything is queued. Advancing the
    // assembler's generations is what makes K=3 a *time* window; skipping
    // the build on an empty cycle would stretch an event's four copies out
    // over however long the next four non-empty cycles happened to span.
    const jamn::proto::NoteBurst burst = assembler_.BuildNextBurst(serviceNowUs_, nextBurstSeq_++);
    if (burst.eventCount == 0) return;

    ByteWriter value(valueBuffer_.data(), valueBuffer_.size());
    if (!jamn::proto::EncodeNoteBurst(burst, value)) {
        ++stats_.packetsTooLarge;
        return;
    }
    std::size_t size = 0;
    if (!FramePacket(MessageType::kNoteBurst, valueBuffer_.data(), value.Position(), size)) {
        ++stats_.packetsTooLarge;
        return;
    }

    BroadcastFramed(Channel::kRealtime, size);
    ++stats_.burstsSent;
}

bool PeerRuntime::FramePacket(MessageType type, const std::uint8_t* value, std::size_t len, std::size_t& outSize) {
    const std::size_t bodyLen = 2 * sizeof(std::uint16_t) + len;  // One TLV header, then the value.
    if (len > 0xFFFF || bodyLen > 0xFFFF) return false;

    jamn::proto::PacketHeader header;
    header.peerId = localPeerId_;
    header.bodyLen = static_cast<std::uint16_t>(bodyLen);

    ByteWriter out(packetBuffer_.data(), packetBuffer_.size());
    if (!jamn::proto::EncodePacketHeader(header, out)) return false;
    if (!jamn::proto::WriteTlvHeader(out, static_cast<std::uint16_t>(type), static_cast<std::uint16_t>(len))) {
        return false;
    }
    if (!out.WriteBytes(value, len)) return false;

    outSize = out.Position();
    return true;
}

void PeerRuntime::SendFramed(PeerId peer, Channel channel, std::size_t size) {
    if (channel == Channel::kRealtime && size > maxRealtimePayload_) {
        // Dropped deliberately. Over the threshold an UNSEQUENCED packet
        // does not degrade to unreliable-fragmented - it becomes reliable,
        // ordered and head-of-line blocking, with no error and no log
        // (enet_transport.h). A dropped realtime packet is the lesser harm.
        ++stats_.packetsTooLarge;
        return;
    }
    if (!transport_.Send(peer, channel, packetBuffer_.data(), size)) ++stats_.sendsFailed;
}

void PeerRuntime::BroadcastFramed(Channel channel, std::size_t size) {
    for (std::size_t i = 0; i < kMaxPeers; ++i) {
        if (!slots_[i].inUse) continue;
        SendFramed(slots_[i].link, channel, size);
    }
}

void PeerRuntime::OnPeerEvent(PeerId peer, PeerEvent event) {
    if (event == PeerEvent::kConnected) {
        AddPeer(peer);
    } else {
        RemovePeer(peer);
    }
    if (peerEventCallback_) peerEventCallback_(peer, event);
}

void PeerRuntime::OnReceive(PeerId from, Channel channel, ByteReader& packet) {
    ++stats_.packetsReceived;

    if (channel != Channel::kRealtime) {
        // Handed out whole and undecoded. Control traffic is the session
        // layer's business, and the runtime forming its own opinion of what
        // a valid control message is would be a second decoder to keep in
        // step with the first.
        if (controlCallback_) controlCallback_(from, packet);
        return;
    }

    const std::size_t slot = FindSlot(from);
    if (slot == kNoSlot) {
        // Realtime traffic from a link with no slot: dropped before a
        // single field is decoded. A peer becomes known through a
        // kConnected event or an explicit AddPeer, never by sending.
        ++stats_.packetsFromUnknownPeer;
        return;
    }
    HandleRealtimePacket(from, slot, packet);
}

void PeerRuntime::HandleRealtimePacket(PeerId from, std::size_t slot, ByteReader& packet) {
    jamn::proto::PacketHeader header;
    if (!jamn::proto::DecodePacketHeader(packet, header)) {
        ++stats_.packetsRejected;
        return;
    }
    // DecodePacketHeader reads the magic without judging it, so both checks
    // belong here. A proto_major mismatch is dropped, never disconnected -
    // that is protocol rule 1, and the refusal is the session layer's call.
    if (header.magic != jamn::proto::kMagic || header.protoMajor != jamn::proto::kCurrentProtoMajor) {
        ++stats_.packetsRejected;
        return;
    }

    ByteReader body(nullptr, 0);
    if (!packet.ReadSlice(body, header.bodyLen)) {
        ++stats_.packetsRejected;
        return;
    }

    const bool wellFramed = jamn::proto::ForEachTlv(body, [&](std::uint16_t type, ByteReader& value) {
        switch (static_cast<MessageType>(type)) {
            case MessageType::kNoteBurst:
                HandleNoteBurst(from, slot, value);
                break;
            case MessageType::kClockPing:
                HandleClockPing(from, value);
                break;
            case MessageType::kClockPong:
                HandleClockPong(slot, value);
                break;
            default:
                break;  // Rule 1: an unrecognised type is skipped, never an error.
        }
    });
    if (!wellFramed) ++stats_.packetsRejected;
}

void PeerRuntime::HandleNoteBurst(PeerId from, std::size_t slot, ByteReader& value) {
    jamn::proto::NoteBurst burst;
    if (!jamn::proto::DecodeNoteBurst(value, burst)) {
        ++stats_.packetsRejected;
        return;
    }

    for (std::size_t i = 0; i < burst.eventCount; ++i) {
        const jamn::proto::NoteEvent& event = burst.events[i];
        if (dedupe_.IsDuplicate(from, event.eventSeq)) {
            ++stats_.notesDeduped;
            continue;
        }

        NoteCrossing::RemoteNote note;
        note.peer = from;
        // The sender's own clock, unconverted: turning it into a local time
        // needs this peer's offset applied on the audio thread, which is
        // where every timestamp conversion lives (docs/CLOCK.md).
        note.remoteSessionTimeUs = burst.baseTSessionUs + event.dtUs;
        note.event = event;

        if (crossing_.Publish(slot, note)) {
            ++stats_.notesDelivered;
        } else {
            // The lane is full - the audio thread has stopped draining, or
            // this peer is flooding. Dropped and counted, never retried:
            // there is no backpressure to apply toward a real-time consumer.
            ++stats_.notesDroppedAtCrossing;
        }
    }
}

void PeerRuntime::HandleClockPing(PeerId from, ByteReader& value) {
    jamn::proto::ClockPing ping;
    if (!jamn::proto::DecodeClockPing(value, ping)) {
        ++stats_.packetsRejected;
        return;
    }

    // t2 and t3 are the same reading because the reply is built right here,
    // inside the Poll that delivered the ping - there is no queue between
    // them to spend time in. ITransport permits sending from a receive
    // callback; SimTransport drains through a swapped-out batch and ENet
    // queues for the next service, so neither re-enters its own iteration.
    jamn::proto::ClockPong pong;
    pong.pingSeq = ping.pingSeq;
    pong.t1 = ping.t1;  // Echoed untouched: an unsequenced channel can deliver
    pong.t2 = serviceNowUs_;  // a pong for a ping the sender has forgotten, so
    pong.t3 = serviceNowUs_;  // the round trip carries its own t1.

    ByteWriter out(valueBuffer_.data(), valueBuffer_.size());
    if (!jamn::proto::EncodeClockPong(pong, out)) return;
    std::size_t size = 0;
    if (!FramePacket(MessageType::kClockPong, valueBuffer_.data(), out.Position(), size)) return;

    SendFramed(from, Channel::kRealtime, size);
    ++stats_.pongsSent;
}

void PeerRuntime::HandleClockPong(std::size_t slot, ByteReader& value) {
    jamn::proto::ClockPong pong;
    if (!jamn::proto::DecodeClockPong(value, pong)) {
        ++stats_.packetsRejected;
        return;
    }

    ClockSyncSample sample;
    sample.t1 = pong.t1;
    sample.t2 = pong.t2;
    sample.t3 = pong.t3;
    sample.t4 = serviceNowUs_;

    slots_[slot].clockSync.AddSample(sample, serviceNowUs_);
    ++stats_.clockSamplesFolded;

    // Published for the audio thread the same way MasterBus publishes gain:
    // a plain atomic store, no lock, no handshake. A reader that catches the
    // previous value simply converts with a slightly older offset.
    //
    // Two independent stores, and deliberately not a pair that establishes
    // any ordering between them: a reader can catch either one a beat
    // stale. Both are hints - the worst case is one block converted with a
    // slightly old offset, or one block declining to convert because the
    // lock flag has not caught up yet. Making these a single atomic struct
    // would buy consistency nothing here consumes.
    slotOffsetUs_[slot].store(slots_[slot].clockSync.EstimatedOffsetUs(), std::memory_order_release);
    slotOffsetLocked_[slot].store(slots_[slot].clockSync.IsLocked(), std::memory_order_release);
}

}  // namespace jamn::engine
