#include "jamn_engine/net_thread.h"

#include <algorithm>
#include <chrono>

namespace jamn::engine {

namespace {

std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

NetThread::NetThread(PeerRuntime& runtime, std::int64_t pollIntervalUs)
    : runtime_(runtime), pollIntervalUs_(std::max<std::int64_t>(pollIntervalUs, 1)) {}

NetThread::~NetThread() {
    Stop();
}

void NetThread::Start() {
    if (started_) return;
    started_ = true;
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this] { Run(); });
}

void NetThread::Stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_relaxed);
}

bool NetThread::TakeCadence(Cadence& out) const {
    if (running_.load(std::memory_order_relaxed)) return false;

    out.iterations = iterations_;
    out.requestedUs = pollIntervalUs_;
    out.minUs = intervalMinUs_;
    out.maxUs = intervalMaxUs_;
    out.meanUs = intervalCount_ > 0 ? intervalSumUs_ / intervalCount_ : 0;
    out.p50Us = intervals_.Percentile(lastRecordUs_, 0.50);
    out.p99Us = intervals_.P99(lastRecordUs_);
    return true;
}

void NetThread::RecordInterval(std::int64_t intervalUs, std::int64_t nowUs) {
    if (intervalCount_ == 0) {
        intervalMinUs_ = intervalUs;
        intervalMaxUs_ = intervalUs;
    } else {
        intervalMinUs_ = std::min(intervalMinUs_, intervalUs);
        intervalMaxUs_ = std::max(intervalMaxUs_, intervalUs);
    }
    ++intervalCount_;
    intervalSumUs_ += intervalUs;
    intervals_.Record(intervalUs, nowUs);
    lastRecordUs_ = nowUs;
}

void NetThread::Run() {
    const auto interval = std::chrono::microseconds(pollIntervalUs_);

    // sleep_until on an absolute schedule, never sleep_for on a relative
    // one: sleep_for overshoots (~1.2x on the dev box), and a relative sleep
    // adds that overshoot to every iteration rather than absorbing it. At
    // 250us a compounding 20% error is the difference between meeting the
    // cadence requirement and missing it by a third.
    //
    // Measured, not assumed: across several runs of two `jamn_app
    // --headless` peers over loopback on the dev box, the achieved mean is
    // 250-251us against a requested 250us, p99 289-362us, shortest single
    // interval 25-87us, longest 2.4-3.4ms. The sub-interval minimum is the
    // schedule catching up rather than jitter - an iteration that overran
    // by 225us leaves the next deadline only 25us away - and that catching
    // up is exactly what holds the mean at the requested figure. The tail
    // is what a general-purpose scheduler gives. Since the clock budget is
    // spent against the achieved number rather than the requested one,
    // TakeCadence reports it instead of a comment asserting it.
    auto next = std::chrono::steady_clock::now();
    std::int64_t previousUs = 0;
    bool havePrevious = false;

    while (!stop_.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        const std::int64_t nowUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        if (havePrevious) RecordInterval(nowUs - previousUs, nowUs);
        previousUs = nowUs;
        havePrevious = true;
        ++iterations_;

        runtime_.Service(nowUs);

        next += interval;
        // A Service call that ran longer than one interval leaves the
        // schedule in the past. Resync rather than catching up: catching up
        // means a burst of zero-length sleeps, which is a busy-wait at
        // exactly the moment the machine is already behind.
        if (next < now) next = now + interval;
        std::this_thread::sleep_until(next);
    }
}

}  // namespace jamn::engine
