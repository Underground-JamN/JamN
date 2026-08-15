#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "jamn_core/histogram64.h"
#include "jamn_engine/peer_runtime.h"

namespace jamn::engine {

// The thread `PeerRuntime::Service` runs on. T4.1 gave the run loop a
// production owner; it did not give it a thread, and until this existed the
// only things that ever drove Service were test binaries and jamn_bench.
//
// **It lives here, not in jamn_app, on purpose.** jamn_app links JUCE, so
// nothing in it is reachable from `ctest -L fast` or from either sanitizer
// preset - both of which are core-only scope. A start/stop/join loop is
// exactly the kind of code TSan is worth running against, so it goes in the
// JUCE-free module that TSan can see, and jamn_app is left owning only the
// lifetime ordering it cannot delegate.
//
// Threading: this class creates the net thread and is the only thing that
// joins it. Every method here belongs to the thread that constructed the
// object (the message thread, in the app) - never to the net thread itself.
// PeerRuntime's own contract is unchanged: Service and everything it
// reaches is the net thread, SubmitLocalEvent is the message thread, and
// the crossing is the audio thread.
class NetThread {
public:
    // 250us, and this number is a requirement rather than a taste.
    // Clock-offset p99 tracks the poll interval at roughly 1:1 - 50us -> 59us,
    // 250us -> 205us, 1ms -> 934us, 4ms -> 2772us, measured on loopback where
    // the true offset is exactly zero - because both peers polling on a fixed
    // period keeps the phase between their loops near-constant, which makes
    // the error a systematic bias rather than noise, and min-RTT selection
    // cannot filter a bias every sample shares. A 1ms loop would spend about
    // twice the whole 500us clock-accuracy budget before the network
    // contributed anything. docs/CLOCK.md carries the measurement and the
    // reasoning; `jamn_bench --backend loopback-clock --poll-interval-us N`
    // reproduces any row of it.
    static constexpr std::int64_t kDefaultPollIntervalUs = 250;

    // What the loop actually achieved, as opposed to what it was asked for.
    // Reported rather than assumed because `sleep_for` overshoots by ~1.2x
    // on the dev box - a requested 250us is not a delivered 250us, and the
    // 1:1 relationship above applies to the delivered one.
    struct Cadence {
        std::uint64_t iterations = 0;
        std::int64_t requestedUs = 0;
        std::int64_t minUs = 0;
        std::int64_t maxUs = 0;
        std::int64_t meanUs = 0;
        // Histogram64 is a 5-second sliding window, so unlike the three
        // above these describe the end of the run, not all of it - and it
        // reports bucket upper bounds, whose spacing is about 25% at this
        // scale. A p50 slightly above the mean is that quantisation, not a
        // contradiction.
        std::int64_t p50Us = 0;
        std::int64_t p99Us = 0;
    };

    explicit NetThread(PeerRuntime& runtime, std::int64_t pollIntervalUs = kDefaultPollIntervalUs);

    // Stops and joins if that has not already happened. Destroying a
    // NetThread is therefore always safe; it is still better for the owner
    // to call Stop() explicitly, so the join is ordered against whatever
    // else it owns rather than against a member's declaration order.
    ~NetThread();

    NetThread(const NetThread&) = delete;
    NetThread& operator=(const NetThread&) = delete;

    // Idempotent. A NetThread that has been stopped cannot be restarted -
    // its cadence counters describe one run, and a second run would either
    // pool two measurements or silently discard the first.
    void Start();

    // Idempotent, and joins before returning. After it returns, no callback
    // this runtime installs can still be in flight - which is the whole
    // reason it exists as a public method rather than only in the
    // destructor: the transport and the runtime may not be destroyed while
    // a thread is still inside Service.
    void Stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }

    // False, writing nothing, while the thread is still running. The
    // counters below are written by the net thread with no synchronisation
    // at all, so a live read is a plain data race - the same race
    // docs/RT_RULES.md makes an ADR trigger for reading Histogram64 out
    // from under the audio thread. Refusing the read is cheaper than
    // making it safe, because nothing needs it live.
    bool TakeCadence(Cadence& out) const;

private:
    void Run();
    void RecordInterval(std::int64_t intervalUs, std::int64_t nowUs);

    PeerRuntime& runtime_;
    std::int64_t pollIntervalUs_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    bool started_ = false;

    // Net thread only, and only until it joins.
    std::uint64_t iterations_ = 0;
    std::int64_t intervalCount_ = 0;
    std::int64_t intervalSumUs_ = 0;
    std::int64_t intervalMinUs_ = 0;
    std::int64_t intervalMaxUs_ = 0;
    std::int64_t lastRecordUs_ = 0;
    jamn::core::Histogram64 intervals_;
};

}  // namespace jamn::engine
