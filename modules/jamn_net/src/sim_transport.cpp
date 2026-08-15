#include "jamn_net/sim_transport.h"

#include <algorithm>
#include <utility>

namespace jamn::net {

bool SimTransport::Send(PeerId peer, Channel channel, const std::uint8_t* data, std::size_t len) {
    return network_.Enqueue(self_, peer, channel, data, len);
}

void SimTransport::SetReceiveCallback(ReceiveCallback callback) { callback_ = std::move(callback); }

void SimTransport::SetPeerEventCallback(PeerEventCallback callback) { peerEventCallback_ = std::move(callback); }

void SimTransport::Disconnect(PeerId peer) { network_.Sever(self_, peer); }

void SimTransport::QueuePacket(PeerId from, Channel channel, std::vector<std::uint8_t> payload) {
    Pending pending;
    pending.peer = from;
    pending.channel = channel;
    pending.payload = std::move(payload);
    ready_.push_back(std::move(pending));
}

void SimTransport::QueuePeerEvent(PeerId peer, PeerEvent event) {
    Pending pending;
    pending.isPeerEvent = true;
    pending.peer = peer;
    pending.event = event;
    ready_.push_back(std::move(pending));
}

void SimTransport::Poll(std::int64_t /*nowUs*/) {
    // Virtual time is SimNetwork's, advanced explicitly by Advance() - a
    // caller's nowUs would be a second, conflicting timebase, so it is
    // deliberately ignored here rather than reconciled.
    //
    // Drained through a swapped-out copy so a callback may safely call
    // Send/Disconnect (which can queue onto ready_) without invalidating
    // the iteration underneath it. Anything queued during this Poll is
    // reported by the next one.
    std::vector<Pending> batch;
    batch.swap(ready_);
    for (auto& pending : batch) {
        if (pending.isPeerEvent) {
            if (peerEventCallback_) peerEventCallback_(pending.peer, pending.event);
        } else if (callback_) {
            jamn::core::ByteReader body(pending.payload.data(), pending.payload.size());
            callback_(pending.peer, pending.channel, body);
        }
    }
}

SimTransport& SimNetwork::CreateNode(PeerId id) {
    // Direct `new`, not std::make_unique: SimTransport's constructor is
    // private to everyone but SimNetwork, and that friendship doesn't
    // extend to make_unique's own internals.
    std::unique_ptr<SimTransport> node(new SimTransport(*this, id));
    SimTransport& ref = *node;
    nodes_[id] = &ref;
    nodeStorage_.push_back(std::move(node));
    return ref;
}

void SimNetwork::Connect(PeerId a, PeerId b) {
    const auto aIt = nodes_.find(a);
    const auto bIt = nodes_.find(b);
    if (aIt == nodes_.end() || bIt == nodes_.end()) return;

    severedLinks_.erase(LinkKey(a, b));
    aIt->second->QueuePeerEvent(b, PeerEvent::kConnected);
    bIt->second->QueuePeerEvent(a, PeerEvent::kConnected);
}

void SimNetwork::Sever(PeerId a, PeerId b) {
    if (!severedLinks_.insert(LinkKey(a, b)).second) return;  // Already severed - no repeat event.

    const auto aIt = nodes_.find(a);
    const auto bIt = nodes_.find(b);
    if (aIt != nodes_.end()) aIt->second->QueuePeerEvent(b, PeerEvent::kDisconnected);
    if (bIt != nodes_.end()) bIt->second->QueuePeerEvent(a, PeerEvent::kDisconnected);
}

bool SimNetwork::Enqueue(PeerId from, PeerId to, Channel channel, const std::uint8_t* data, std::size_t len) {
    if (nodes_.find(to) == nodes_.end()) return false;                    // No route.
    if (severedLinks_.find(LinkKey(from, to)) != severedLinks_.end()) return false;  // Link torn down.

    const LinkConfig config = ConfigFor(from, to);
    const bool reliable = ChannelIsReliable(channel);
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    // A reliable channel draws nothing from the PRNG at all, rather than
    // drawing and discarding: a control message must not perturb the draw
    // sequence the realtime channel's loss/jitter pattern depends on, or
    // adding one join handshake would reshuffle every seeded sim result.
    if (!reliable && unit(rng_) < config.lossProbability) {
        return true;  // Handed to the local stack; lost in flight - not a Send() failure.
    }

    auto scheduleOne = [&]() {
        std::int64_t delay = config.delayUs;
        // Flat delay on a reliable channel: arrival times then rise
        // monotonically with send order, and Advance's (arrivalTime,
        // sendSeq) ordering makes in-order delivery follow from that.
        if (!reliable && config.jitterUs > 0) {
            std::uniform_int_distribution<std::int64_t> jitterDist(-config.jitterUs, config.jitterUs);
            delay += jitterDist(rng_);
        }
        if (delay < 0) delay = 0;

        InFlightPacket pkt;
        pkt.arrivalTime = clock_.nowUs() + delay;
        pkt.sendSeq = nextSendSeq_++;
        pkt.from = from;
        pkt.to = to;
        pkt.channel = channel;
        pkt.payload.assign(data, data + len);
        inFlight_.push_back(std::move(pkt));
    };

    scheduleOne();
    if (!reliable && unit(rng_) < config.duplicateProbability) {
        scheduleOne();
    }
    return true;
}

void SimNetwork::Advance(std::int64_t deltaUs) {
    clock_.Advance(deltaUs);
    const jamn::core::SessionTime now = clock_.nowUs();

    std::vector<InFlightPacket> deliverNow;
    std::vector<InFlightPacket> stillWaiting;
    deliverNow.reserve(inFlight_.size());
    stillWaiting.reserve(inFlight_.size());
    for (auto& pkt : inFlight_) {
        if (pkt.arrivalTime <= now) {
            deliverNow.push_back(std::move(pkt));
        } else {
            stillWaiting.push_back(std::move(pkt));
        }
    }
    inFlight_ = std::move(stillWaiting);

    // Ascending arrival time, send-sequence as the tie-break - the same
    // ordering rule regardless of how the vector happened to be built, so
    // delivery order depends only on (seed, call sequence), never on
    // container/iteration incidentals.
    std::sort(deliverNow.begin(), deliverNow.end(), [](const InFlightPacket& a, const InFlightPacket& b) {
        if (a.arrivalTime != b.arrivalTime) return a.arrivalTime < b.arrivalTime;
        return a.sendSeq < b.sendSeq;
    });

    for (auto& pkt : deliverNow) {
        const auto it = nodes_.find(pkt.to);
        if (it == nodes_.end()) continue;  // Node removed since send - drop silently.
        // Severed since send: a torn-down link drops what was still in
        // flight over it, as a real one does - it does not deliver the
        // backlog after the disconnect has already been reported.
        if (severedLinks_.find(LinkKey(pkt.from, pkt.to)) != severedLinks_.end()) continue;
        it->second->QueuePacket(pkt.from, pkt.channel, std::move(pkt.payload));
    }
}

}  // namespace jamn::net
