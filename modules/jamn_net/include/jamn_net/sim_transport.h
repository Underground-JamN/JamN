#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "jamn_core/sim_clock.h"
#include "jamn_net/transport.h"

namespace jamn::net {

// Per-direction link characteristics: (from, to) need not match (to, from).
// Loss, jitter and duplication apply to Channel::kRealtime only - kControl
// and kBulk are reliable and ordered per docs/PROTOCOL.md's "Transport"
// table, so the sim honours that contract too. Without that split,
// sim-green and real-red diverge the moment a control message rides a lossy
// link: EnetTransport would deliver it and SimTransport would not.
struct LinkConfig {
    std::int64_t delayUs = 0;
    // Actual delay for a given kRealtime packet is delayUs +/- uniform(0,
    // jitterUs) - this is what makes "30 +/- 10ms jitter" a direct reading
    // of (delayUs=30000, jitterUs=10000). Reordering is not a separate
    // knob: it emerges from jitter alone, since delivery is ordered by
    // arrival time, not send time. Reliable channels take delayUs flat, so
    // they never reorder.
    std::int64_t jitterUs = 0;
    double lossProbability = 0.0;
    double duplicateProbability = 0.0;
};

// Whether a channel carries the reliable, ordered contract
// docs/PROTOCOL.md's "Transport" table assigns it. Free function rather
// than a LinkConfig member: it is a property of the protocol's channel
// model, identical for every link, not something a test may configure away.
inline bool ChannelIsReliable(Channel channel) {
    return channel == Channel::kControl || channel == Channel::kBulk;
}

class SimNetwork;

// One node's endpoint into a SimNetwork - implements ITransport by handing
// every Send() to the network's in-flight queue instead of a real socket.
// Only SimNetwork::CreateNode constructs one.
//
// Threading contract: there are no threads. Every callback fires inside
// Poll, on the calling thread, per ITransport::Poll's contract - Advance()
// only moves packets from flight into this node's ready queue, it never
// calls a callback itself.
class SimTransport : public ITransport {
public:
    bool Send(PeerId peer, Channel channel, const std::uint8_t* data, std::size_t len) override;
    void Poll(std::int64_t nowUs) override;
    void SetReceiveCallback(ReceiveCallback callback) override;
    void SetPeerEventCallback(PeerEventCallback callback) override;
    void Disconnect(PeerId peer) override;

private:
    friend class SimNetwork;
    SimTransport(SimNetwork& network, PeerId self) : network_(network), self_(self) {}

    // One queued delivery: either a received packet or a peer event. Kept
    // in a single queue rather than two so a packet that arrived before a
    // disconnect is still reported before it - two queues would silently
    // reorder them relative to each other.
    struct Pending {
        bool isPeerEvent = false;
        PeerId peer = 0;
        PeerEvent event = PeerEvent::kConnected;
        Channel channel = Channel::kRealtime;
        std::vector<std::uint8_t> payload;
    };

    void QueuePacket(PeerId from, Channel channel, std::vector<std::uint8_t> payload);
    void QueuePeerEvent(PeerId peer, PeerEvent event);

    SimNetwork& network_;
    PeerId self_;
    ReceiveCallback callback_;
    PeerEventCallback peerEventCallback_;
    std::vector<Pending> ready_;
};

// Owns N in-process SimTransport nodes, a SimClock, a seeded PRNG, and the
// in-flight packet queue. Single-threaded and driven entirely by explicit
// Advance() calls - there is no background thread and no real I/O anywhere
// in this class, which is what makes it usable as a deterministic,
// hardware-free test harness (docs: "verifiable on one machine with no
// hardware").
class SimNetwork {
public:
    explicit SimNetwork(std::uint64_t seed) : rng_(seed) {}

    SimTransport& CreateNode(PeerId id);

    void SetLinkConfig(PeerId from, PeerId to, LinkConfig config) { linkConfigs_[{from, to}] = config; }

    // Brings the link between two nodes up, queueing a kConnected peer
    // event for each to report from its next Poll. Nodes are routable from
    // the moment they are created, so this is not a precondition for
    // Send - it exists so a test can exercise the peer-event path, and so
    // a link severed by Disconnect can be restored. This lives on
    // SimNetwork rather than on ITransport for the same reason
    // EnetTransport::Connect does: establishing a link needs something only
    // a concrete transport has (an address here, a whole network there).
    void Connect(PeerId a, PeerId b);

    // Advances the clock by deltaUs and moves every in-flight packet whose
    // scheduled arrival time is now <= the new time into its destination
    // node's ready queue, strictly in ascending arrival-time order (ties
    // broken by send sequence, so delivery order is fully deterministic for
    // a given seed and call sequence, never dependent on container
    // iteration order). Nothing is handed to a callback here - each node's
    // own Poll does that.
    void Advance(std::int64_t deltaUs);

    jamn::core::SessionTime Now() const { return clock_.nowUs(); }

private:
    friend class SimTransport;

    // Returns false only when the destination node is unknown to this
    // network or the link to it has been severed by Disconnect (no
    // route) - loss/duplication happen invisibly in flight afterward,
    // exactly as a real UDP send() succeeding locally says nothing about
    // eventual delivery.
    bool Enqueue(PeerId from, PeerId to, Channel channel, const std::uint8_t* data, std::size_t len);

    // Severs the link both ways and queues a kDisconnected peer event on
    // each end. Called by SimTransport::Disconnect.
    void Sever(PeerId a, PeerId b);

    struct InFlightPacket {
        jamn::core::SessionTime arrivalTime;
        std::uint64_t sendSeq = 0;
        PeerId from = 0;
        PeerId to = 0;
        Channel channel = Channel::kRealtime;
        std::vector<std::uint8_t> payload;
    };

    LinkConfig ConfigFor(PeerId from, PeerId to) const {
        const auto it = linkConfigs_.find({from, to});
        return it == linkConfigs_.end() ? LinkConfig{} : it->second;
    }

    // Normalized so a link severed as (a, b) is also severed as (b, a) -
    // a torn-down link is not a one-way condition.
    static std::pair<PeerId, PeerId> LinkKey(PeerId a, PeerId b) {
        return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    }

    jamn::core::SimClock clock_;
    std::mt19937_64 rng_;
    std::uint64_t nextSendSeq_ = 0;
    std::map<std::pair<PeerId, PeerId>, LinkConfig> linkConfigs_;
    std::map<PeerId, SimTransport*> nodes_;
    std::vector<std::unique_ptr<SimTransport>> nodeStorage_;
    std::vector<InFlightPacket> inFlight_;
    std::set<std::pair<PeerId, PeerId>> severedLinks_;
};

}  // namespace jamn::net
