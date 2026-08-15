#include "jamn_engine/audio_clock.h"

#include <cmath>

namespace jamn::engine {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNsPerSecond = 1'000'000'000.0;

}  // namespace

void AudioClock::Prepare(double nominalSampleRate, int blockFrames, double bandwidthHz) {
    nominalSampleRate_ = nominalSampleRate;
    blockFrames_ = blockFrames;

    // A device that reports nothing usable leaves the loop inert rather
    // than dividing by zero on the audio thread. Every query then answers
    // as it does before the first Update.
    if (nominalSampleRate <= 0.0 || blockFrames <= 0 || bandwidthHz <= 0.0) {
        b_ = c_ = 0.0;
        lockAfterSeconds_ = 0.0;
        haveFirst_ = false;
        updateCount_ = 0;
        estimatedSampleRate_ = 0.0;
        return;
    }

    const double period = static_cast<double>(blockFrames) / nominalSampleRate;
    const double omega = 2.0 * kPi * bandwidthHz * period;
    b_ = std::sqrt(2.0) * omega;
    c_ = omega * omega;
    lockAfterSeconds_ = kLockTimeConstants / (2.0 * kPi * bandwidthHz);

    // A Prepare is a discontinuity by definition, so it discards the loop
    // rather than carrying it across. Restarting a device onto a new
    // timeline and keeping the old phase is exactly the step this class
    // promises not to take.
    haveFirst_ = false;
    updateCount_ = 0;
    baseNs_ = 0;
    t0_ = t1_ = e2_ = 0.0;
    samples_ = 0;
    estimatedSampleRate_ = nominalSampleRate;
}

void AudioClock::Update(jamn::core::SampleTime cumulativeSamples, std::int64_t steadyNs) {
    if (b_ == 0.0) {
        return;
    }

    const double period = static_cast<double>(blockFrames_) / nominalSampleRate_;

    if (!haveFirst_) {
        // The first callback establishes the origin and nothing else -
        // there is no interval to measure yet, so the loop starts from the
        // rate the driver claims and spends lockAfterSeconds_ correcting
        // it. IsLocked() is what keeps a caller from believing it early.
        baseNs_ = steadyNs;
        t0_ = 0.0;
        t1_ = period;
        e2_ = period;
        samples_ = cumulativeSamples.samples();
        haveFirst_ = true;
        updateCount_ = 1;
        estimatedSampleRate_ = static_cast<double>(blockFrames_) / e2_;
        return;
    }

    // Adriaensen's second-order DLL. e is how far this callback landed
    // from where the loop predicted; t0_ becomes that prediction (the
    // filtered time), t1_ the next one, and e2_ the filtered period. The
    // filtered time is what the mapping uses, which is why block-to-block
    // scheduling jitter does not reach the scheduler.
    const double t = static_cast<double>(steadyNs - baseNs_) / kNsPerSecond;
    const double e = t - t1_;
    t0_ = t1_;
    t1_ += b_ * e + e2_;
    e2_ += c_ * e;

    samples_ = cumulativeSamples.samples();
    ++updateCount_;
    estimatedSampleRate_ = static_cast<double>(blockFrames_) / e2_;
}

bool AudioClock::IsLocked() const {
    return haveFirst_ && t0_ >= lockAfterSeconds_;
}

double AudioClock::DriftPpm() const {
    if (!haveFirst_ || nominalSampleRate_ <= 0.0) {
        return 0.0;
    }
    return (estimatedSampleRate_ / nominalSampleRate_ - 1.0) * 1e6;
}

jamn::core::SampleTime AudioClock::SamplePositionAt(std::int64_t localUs) const {
    if (!haveFirst_) {
        return jamn::core::SampleTime(0);
    }
    const double t = static_cast<double>(localUs * 1000 - baseNs_) / kNsPerSecond;
    const double offset = (t - t0_) * estimatedSampleRate_;
    return jamn::core::SampleTime(samples_ + static_cast<std::int64_t>(std::llround(offset)));
}

std::int64_t AudioClock::LocalUsAt(jamn::core::SampleTime position) const {
    if (!haveFirst_) {
        return 0;
    }
    const double aheadSeconds = static_cast<double>(position.samples() - samples_) / estimatedSampleRate_;
    const std::int64_t ns = baseNs_ + std::llround((t0_ + aheadSeconds) * kNsPerSecond);
    // Round rather than truncate: this is the inverse of the function
    // above, and a truncating microsecond conversion would make a
    // round-trip through the pair lose up to a microsecond every time.
    return (ns >= 0) ? (ns + 500) / 1000 : (ns - 500) / 1000;
}

}  // namespace jamn::engine
