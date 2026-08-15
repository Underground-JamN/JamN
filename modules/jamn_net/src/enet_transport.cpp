#include "jamn_net/enet_transport.h"

#include <enet/enet.h>

#include <mutex>
#include <utility>

namespace jamn::net {
namespace {

// enet_initialize()/enet_deinitialize() are process-global, not per-host,
// so several EnetTransports in one binary (every test case in
// jamn_net_enet_tests, for one) must not each initialize and tear down the
// library underneath each other. Refcounted, with a mutex because nothing
// promises transports are constructed on one thread - this is a
// construction-time cost, never on a send or poll path.
class EnetLibrary {
public:
    static bool Acquire() {
        std::lock_guard<std::mutex> lock(Mutex());
        if (RefCount() == 0 && enet_initialize() != 0) return false;
        ++RefCount();
        return true;
    }

    static void Release() {
        std::lock_guard<std::mutex> lock(Mutex());
        if (RefCount() == 0) return;
        if (--RefCount() == 0) enet_deinitialize();
    }

private:
    static std::mutex& Mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static int& RefCount() {
        static int count = 0;
        return count;
    }
};

// docs/PROTOCOL.md's "Transport" table, as ENet flags. kRealtime must pass
// UNSEQUENCED explicitly; ENet's plain unreliable is unreliable-*sequenced*
// and silently discards stale packets.
enet_uint32 FlagsFor(Channel channel) {
    switch (channel) {
        case Channel::kControl:
            return ENET_PACKET_FLAG_RELIABLE;
        case Channel::kBulk:
            return ENET_PACKET_FLAG_RELIABLE;
        case Channel::kRealtime:
            return ENET_PACKET_FLAG_UNSEQUENCED;
    }
    return ENET_PACKET_FLAG_RELIABLE;
}

// The channel ids in docs/PROTOCOL.md's table are the ENet channel ids -
// Channel's underlying values are that table's numbering, so this is a cast
// rather than a mapping. kChannelCount is one past the last channel this
// protocol uses; the "3+ Audio" row is a later phase's.
constexpr std::size_t kChannelCount = 3;

std::size_t FragmentThreshold(const ENetPeer* peer) {
    // The same arithmetic enet_peer_send does before deciding to fragment
    // (peer.c), read from the live peer because ENet negotiates the MTU
    // down on connect rather than always using ENET_HOST_DEFAULT_MTU.
    const std::size_t overhead = sizeof(ENetProtocolHeader) + sizeof(ENetProtocolSendFragment);
    if (peer->mtu <= overhead) return 0;
    std::size_t threshold = peer->mtu - overhead;
    if (peer->host != nullptr && peer->host->checksum != nullptr) {
        if (threshold <= sizeof(enet_uint32)) return 0;
        threshold -= sizeof(enet_uint32);
    }
    return threshold;
}

}  // namespace

struct EnetTransport::Impl {
    ENetHost* host = nullptr;
    bool libraryAcquired = false;
    ReceiveCallback receiveCallback;
    PeerEventCallback peerEventCallback;

    ~Impl() {
        if (host != nullptr) enet_host_destroy(host);
        if (libraryAcquired) EnetLibrary::Release();
    }

    // A link's PeerId is its slot index in this host's peer array - stable
    // for the life of the link and unique per host, which is exactly what a
    // routing identity needs.
    PeerId IdOf(const ENetPeer* peer) const { return static_cast<PeerId>(peer - host->peers); }

    ENetPeer* PeerOf(PeerId id) const {
        if (host == nullptr || id >= host->peerCount) return nullptr;
        ENetPeer* peer = &host->peers[id];
        return peer->state == ENET_PEER_STATE_CONNECTED ? peer : nullptr;
    }
};

EnetTransport::EnetTransport() : impl_(std::make_unique<Impl>()) {
    impl_->libraryAcquired = EnetLibrary::Acquire();
}

EnetTransport::~EnetTransport() = default;

bool EnetTransport::Listen(std::uint16_t port, std::size_t maxPeers, bool bindLoopbackOnly) {
    if (!impl_->libraryAcquired || impl_->host != nullptr) return false;

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    if (bindLoopbackOnly && enet_address_set_host_ip(&address, "127.0.0.1") != 0) return false;

    impl_->host = enet_host_create(&address, maxPeers, kChannelCount, 0, 0);
    return impl_->host != nullptr;
}

bool EnetTransport::Connect(const char* hostName, std::uint16_t port) {
    if (!impl_->libraryAcquired) return false;

    if (impl_->host == nullptr) {
        impl_->host = enet_host_create(nullptr, 1, kChannelCount, 0, 0);
        if (impl_->host == nullptr) return false;
    }

    ENetAddress address;
    address.port = port;
    if (enet_address_set_host_ip(&address, hostName) != 0) {
        // Not an IP literal - fall back to a name lookup.
        if (enet_address_set_host(&address, hostName) != 0) return false;
    }

    return enet_host_connect(impl_->host, &address, kChannelCount, 0) != nullptr;
}

bool EnetTransport::IsOpen() const { return impl_->host != nullptr; }

std::size_t EnetTransport::MaxRealtimePayloadFor(PeerId peer) const {
    const ENetPeer* enetPeer = impl_->PeerOf(peer);
    return enetPeer == nullptr ? 0 : FragmentThreshold(enetPeer);
}

bool EnetTransport::Send(PeerId peer, Channel channel, const std::uint8_t* data, std::size_t len) {
    ENetPeer* enetPeer = impl_->PeerOf(peer);
    if (enetPeer == nullptr) return false;

    // Refuse rather than hand an oversized unsequenced payload to ENet and
    // trust the flags to protect it - see MaxRealtimePayloadFor's comment
    // for why they don't. Reliable channels are allowed to fragment: that
    // is what "reliable, fragmented" means for kBulk, and kControl's
    // messages are small enough that fragmenting one is not a silent
    // downgrade of anything.
    if (channel == Channel::kRealtime && len > FragmentThreshold(enetPeer)) return false;

    ENetPacket* packet = enet_packet_create(data, len, FlagsFor(channel));
    if (packet == nullptr) return false;
    if (enet_peer_send(enetPeer, static_cast<enet_uint8>(channel), packet) != 0) {
        enet_packet_destroy(packet);  // Ownership only transfers on success.
        return false;
    }
    return true;
}

void EnetTransport::SetReceiveCallback(ReceiveCallback callback) {
    impl_->receiveCallback = std::move(callback);
}

void EnetTransport::SetPeerEventCallback(PeerEventCallback callback) {
    impl_->peerEventCallback = std::move(callback);
}

void EnetTransport::Disconnect(PeerId peer) {
    ENetPeer* enetPeer = impl_->PeerOf(peer);
    if (enetPeer == nullptr) return;
    enet_peer_disconnect(enetPeer, 0);
}

void EnetTransport::Poll(std::int64_t /*nowUs*/) {
    // ENet keeps its own millisecond timebase internally; the caller's
    // nowUs would be a second, conflicting one, so it is deliberately
    // ignored here rather than reconciled. It stays in ITransport's
    // signature because SimTransport's virtual-time harness and a future
    // implementation that does schedule against the caller's clock both
    // need it.
    if (impl_->host == nullptr) return;

    ENetEvent event;
    // Timeout 0: service what is already there and return. Blocking here
    // would stall whatever thread owns the poll loop.
    while (enet_host_service(impl_->host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                if (impl_->peerEventCallback) {
                    impl_->peerEventCallback(impl_->IdOf(event.peer), PeerEvent::kConnected);
                }
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
                if (impl_->peerEventCallback) {
                    impl_->peerEventCallback(impl_->IdOf(event.peer), PeerEvent::kDisconnected);
                }
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                if (impl_->receiveCallback && event.channelID < kChannelCount) {
                    jamn::core::ByteReader body(event.packet->data, event.packet->dataLength);
                    impl_->receiveCallback(impl_->IdOf(event.peer), static_cast<Channel>(event.channelID), body);
                }
                enet_packet_destroy(event.packet);
                break;
            }

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
}

}  // namespace jamn::net
