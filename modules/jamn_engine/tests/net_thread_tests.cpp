// NetThread's own contract: it starts one thread, drives Service on an
// absolute schedule, and joins before anything it borrows can be destroyed.
//
// Deliberately driven against a counting stub rather than SimTransport: a
// SimNetwork would have to be Advance()d from this thread while the net
// thread polls it, which is a race in the harness, not in the code under
// test. What matters here is that Service is called, at roughly the right
// rate, and that stopping is exact - none of which needs delivery.
#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "jamn_engine/net_thread.h"
#include "jamn_net/transport.h"

namespace {

using jamn::engine::NetThread;
using jamn::engine::PeerRuntime;

class CountingTransport final : public jamn::net::ITransport {
public:
    bool Send(jamn::net::PeerId, jamn::net::Channel, const std::uint8_t*, std::size_t) override { return true; }
    void Poll(std::int64_t) override { polls.fetch_add(1, std::memory_order_relaxed); }
    void SetReceiveCallback(ReceiveCallback) override {}
    void SetPeerEventCallback(PeerEventCallback) override {}
    void Disconnect(jamn::net::PeerId) override {}

    std::atomic<std::uint64_t> polls{0};
};

// Long enough that a 250us loop turns in hundreds of iterations, short
// enough not to slow the fast suite noticeably.
constexpr auto kRunFor = std::chrono::milliseconds(50);

}  // namespace

TEST_CASE("NetThread drives Service until it is stopped", "[net_thread]") {
    CountingTransport transport;
    PeerRuntime runtime(transport);
    NetThread net(runtime);

    REQUIRE_FALSE(net.running());
    net.Start();
    REQUIRE(net.running());

    std::this_thread::sleep_for(kRunFor);
    net.Stop();
    REQUIRE_FALSE(net.running());

    const std::uint64_t afterStop = transport.polls.load();
    REQUIRE(afterStop > 1);

    // Stop joined rather than merely signalled: nothing may still be inside
    // Service once it has returned, or the runtime and the transport cannot
    // safely be destroyed after it.
    std::this_thread::sleep_for(kRunFor);
    REQUIRE(transport.polls.load() == afterStop);
}

TEST_CASE("NetThread reports the cadence it achieved, not the one it asked for", "[net_thread]") {
    CountingTransport transport;
    PeerRuntime runtime(transport);
    NetThread net(runtime, /*pollIntervalUs=*/250);

    NetThread::Cadence cadence;
    net.Start();
    // Refused while running: these counters are written by the net thread
    // with no synchronisation, so the only safe read is after the join.
    REQUIRE_FALSE(net.TakeCadence(cadence));

    std::this_thread::sleep_for(kRunFor);
    net.Stop();

    REQUIRE(net.TakeCadence(cadence));
    REQUIRE(cadence.requestedUs == 250);
    REQUIRE(cadence.iterations > 1);
    REQUIRE(cadence.minUs > 0);
    REQUIRE(cadence.maxUs >= cadence.minUs);
    REQUIRE(cadence.meanUs >= cadence.minUs);
    REQUIRE(cadence.meanUs <= cadence.maxUs);
    // A deliberately loose ceiling. The point of asserting at all is to
    // catch a loop that is not sleeping on the requested schedule (a
    // seconds-long interval, or none at all); the actual achieved number is
    // a property of the machine and is reported rather than asserted, since
    // a busy or virtualised box can miss any tight bound legitimately.
    REQUIRE(cadence.meanUs < 50'000);
}

TEST_CASE("NetThread joins from its destructor when the owner never stops it", "[net_thread]") {
    CountingTransport transport;
    PeerRuntime runtime(transport);

    {
        NetThread net(runtime);
        net.Start();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Reaching here at all is most of the assertion: an un-joined thread
    // would still be polling a destroyed NetThread's runtime reference.
    const std::uint64_t afterDestruction = transport.polls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(transport.polls.load() == afterDestruction);
}

TEST_CASE("NetThread Start and Stop are both idempotent", "[net_thread]") {
    CountingTransport transport;
    PeerRuntime runtime(transport);
    NetThread net(runtime);

    net.Start();
    net.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    net.Stop();
    net.Stop();
    REQUIRE_FALSE(net.running());

    // Start after Stop stays stopped: one NetThread measures one run, and a
    // second run would either pool two cadences or discard the first.
    net.Start();
    REQUIRE_FALSE(net.running());
}

TEST_CASE("NetThread that was never started stops and reports cleanly", "[net_thread]") {
    CountingTransport transport;
    PeerRuntime runtime(transport);
    NetThread net(runtime);

    net.Stop();
    NetThread::Cadence cadence;
    REQUIRE(net.TakeCadence(cadence));
    REQUIRE(cadence.iterations == 0);
    REQUIRE(cadence.meanUs == 0);
    REQUIRE(transport.polls.load() == 0);
}
