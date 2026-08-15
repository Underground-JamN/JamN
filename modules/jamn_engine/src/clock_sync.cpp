#include "jamn_engine/clock_sync.h"

namespace jamn::engine {

bool ClockSync::ShouldPing(std::int64_t nowUs, std::int64_t sessionStartUs, std::int64_t lastPingUs) {
    const std::int64_t elapsedSinceStart = nowUs - sessionStartUs;
    const std::int64_t period = (elapsedSinceStart < kFastPhaseDurationUs) ? kFastPeriodUs : kSlowPeriodUs;
    return (nowUs - lastPingUs) >= period;
}

std::int64_t ClockSync::MinRttOffsetUs() const {
    std::int64_t bestRtt = window_[0].sample.Rtt();
    std::int64_t bestOffset = window_[0].sample.Offset();
    std::uint64_t bestSeq = window_[0].seq;
    for (std::size_t i = 1; i < sampleCount_; ++i) {
        const std::int64_t rtt = window_[i].sample.Rtt();
        // <= , not < : on a tied RTT, the more recently inserted sample
        // wins, so a stable-RTT link still tracks a genuine offset change
        // instead of staying pinned to whichever sample happened to be
        // first.
        if (rtt < bestRtt || (rtt == bestRtt && window_[i].seq > bestSeq)) {
            bestRtt = rtt;
            bestOffset = window_[i].sample.Offset();
            bestSeq = window_[i].seq;
        }
    }
    return bestOffset;
}

std::int64_t ClockSync::FitSkewPpm() const {
    if (sampleCount_ < 2) return 0;

    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    const std::int64_t x0 = window_[0].localTimeUs;  // Recenter for numerical stability.
    for (std::size_t i = 0; i < sampleCount_; ++i) {
        const double x = static_cast<double>(window_[i].localTimeUs - x0);
        const double y = static_cast<double>(window_[i].sample.Offset());
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
    }
    const double n = static_cast<double>(sampleCount_);
    const double denom = n * sumXX - sumX * sumX;
    if (denom == 0.0) return 0;  // No time spread to fit a slope against.

    // Both offset and localTime are microseconds, so the raw slope is
    // already a dimensionless ratio (us drift per us elapsed) - multiplying
    // by 1e6 converts that ratio directly to parts-per-million.
    const double slope = (n * sumXY - sumX * sumY) / denom;
    return static_cast<std::int64_t>(slope * 1'000'000.0);
}

void ClockSync::ResetWindow() {
    windowNext_ = 0;
    sampleCount_ = 0;
    slewedOffsetUs_ = 0;
    skewPpm_ = 0;
}

void ClockSync::AddSample(const ClockSyncSample& sample, std::int64_t nowUs) {
    if (IsLocked()) {
        const std::int64_t error = sample.Offset() - slewedOffsetUs_;
        if (error > kReLockThresholdUs || error < -kReLockThresholdUs) {
            // Large discontinuity (a laptop waking from sleep, a host
            // restarting) - the caller flushes whatever it considers
            // "held" before we discard the window and start over.
            if (reLockCallback_) reLockCallback_();
            ResetWindow();
            haveLastUpdate_ = false;
        }
    }

    WindowEntry entry;
    entry.sample = sample;
    entry.localTimeUs = nowUs;
    entry.seq = nextSeq_++;
    window_[windowNext_] = entry;
    windowNext_ = (windowNext_ + 1) % kWindowSize;
    if (sampleCount_ < kWindowSize) ++sampleCount_;

    const std::int64_t target = MinRttOffsetUs();
    skewPpm_ = FitSkewPpm();

    if (!IsLocked() || !haveLastUpdate_) {
        // Not locked yet (still converging), or just (re-)became locked
        // this call: track the current best estimate directly. "Lock,
        // then slew - never step" governs updates once already locked and
        // established, not the initial convergence into that state.
        slewedOffsetUs_ = target;
        lastUpdateUs_ = nowUs;
        haveLastUpdate_ = true;
        return;
    }

    const std::int64_t elapsedUs = nowUs - lastUpdateUs_;
    lastUpdateUs_ = nowUs;
    if (elapsedUs <= 0) return;

    // Feed-forward: the drift predicted since the last update, applied
    // unconditionally - it's a prediction, not a correction, so it isn't
    // subject to the slew-rate cap below.
    const std::int64_t predictedDriftUs = (skewPpm_ * elapsedUs) / 1'000'000;

    const std::int64_t error = target - (slewedOffsetUs_ + predictedDriftUs);
    const std::int64_t maxStepUs = (kMaxSlewPpm * elapsedUs) / 1'000'000;
    std::int64_t step = error;
    if (step > maxStepUs) step = maxStepUs;
    if (step < -maxStepUs) step = -maxStepUs;

    slewedOffsetUs_ += predictedDriftUs + step;
}

void ClockSync::SetManualOffsetUs(std::int64_t offsetUs) {
    if (offsetUs > kManualOffsetLimitUs) offsetUs = kManualOffsetLimitUs;
    if (offsetUs < -kManualOffsetLimitUs) offsetUs = -kManualOffsetLimitUs;
    manualOffsetUs_ = offsetUs;
}

}  // namespace jamn::engine
