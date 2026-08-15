#pragma once

#include <cstddef>
#include <cstdint>

#include "jamn_core/time_types.h"

namespace jamn::engine {

// Clock 2 (docs/CLOCK.md): the local device's sample clock against the
// local steady clock. Fed one (cumulative samples, steady-clock time at
// callback entry) pair per block, on the audio thread, at block start.
//
// It exists because the audio device's crystal is not the CPU's crystal. A
// device that reports 48000 is routinely off by tens or hundreds of ppm,
// and callback entry times are jittery relative to wall-clock time. Taking
// the device's word for its rate puts remote events a few milliseconds off
// even when Clock 1 is exactly right; taking the raw callback timestamps
// at face value instead makes the mapping jitter by a block. This filters
// the second problem to measure the first.
//
// The filter is Adriaensen's second-order DLL ("Using a DLL to filter
// time"): a phase-locked loop over callback arrivals, with a period
// estimate as its second state variable. It is chosen over a least-squares
// fit for the property this code needs most - it cannot step. The output
// moves continuously by construction, so a scheduler mapping session time
// to sample positions through it never sees the timeline jump, which is
// what a stuck or double-triggered note would come from.
//
// Threading: audio thread only, and it holds no synchronisation of its
// own. Update and every query below are pure arithmetic on members - no
// allocation, no locks, no branches on anything unbounded - so all of it
// is callable inside a RealtimeScope.
class AudioClock {
public:
    // The loop bandwidth. 0.1Hz is a ~1.6s time constant: slow enough that
    // per-callback scheduling jitter is averaged away almost entirely,
    // fast enough to follow a real crystal, whose drift moves over minutes
    // rather than seconds. Lower is not better - it only makes the initial
    // convergence longer while buying accuracy the crystal will not hold
    // still for.
    static constexpr double kDefaultBandwidthHz = 0.1;

    // How long the estimate is treated as untrustworthy. The loop starts
    // from the device's nominal rate, so until it has converged the rate
    // error is exactly the error the class exists to remove. Four time
    // constants is ~98% of the way there.
    static constexpr double kLockTimeConstants = 4.0;

    // Call before the first Update, and again on every device restart - a
    // restart is a new sample timeline at a possibly different rate, and
    // feeding the old loop across the discontinuity is precisely the step
    // this class promises never to take. Not real-time-constrained; it is
    // the audioDeviceAboutToStart moment, not a callback.
    void Prepare(double nominalSampleRate, int blockFrames, double bandwidthHz = kDefaultBandwidthHz);

    // One block's arrival. cumulativeSamples is the index of this block's
    // first frame - the count of frames delivered *before* it - and
    // steadyNs is steady_clock at callback entry, on the same timebase the
    // rest of the app reads local time from.
    void Update(jamn::core::SampleTime cumulativeSamples, std::int64_t steadyNs);

    // Whether the estimate is worth using. Means the same thing here as
    // ClockSync::IsLocked does there: the number below is trustworthy, not
    // merely present. Queries before this still answer - a caller with
    // nothing better to do than approximate is not helped by a failure -
    // but they answer at close to the nominal rate.
    bool IsLocked() const;

    // The device's measured rate in Hz, which is the whole point: it is
    // not the rate the driver reported, and the difference is the error
    // this removes.
    double EstimatedSampleRate() const { return estimatedSampleRate_; }

    // Drift of the measured rate from the nominal one, in parts per
    // million - the same unit ClockSync reports skew in, and the number to
    // look at when asking whether this is doing anything.
    double DriftPpm() const;

    std::size_t UpdateCount() const { return updateCount_; }

    // Where a local steady-clock time lands in the device's sample stream,
    // and its inverse. Microseconds in and out rather than nanoseconds,
    // because a caller reaches these holding a local time resolved through
    // ClockSync, which is a microsecond quantity - the nanoseconds are only
    // needed on Update's input side, where they are the raw measurement.
    //
    // Both extrapolate freely on either side of the last block. That is
    // deliberate: the scheduler asks about events a jitter-buffer depth
    // into the future, which is always past the last callback seen.
    jamn::core::SampleTime SamplePositionAt(std::int64_t localUs) const;
    std::int64_t LocalUsAt(jamn::core::SampleTime position) const;

private:
    double nominalSampleRate_ = 0.0;
    int blockFrames_ = 0;
    // The DLL's two coefficients, from bandwidth and block period:
    // b = sqrt(2)*w, c = w*w, with w = 2*pi*bandwidth*period.
    double b_ = 0.0;
    double c_ = 0.0;
    double lockAfterSeconds_ = 0.0;

    bool haveFirst_ = false;
    std::size_t updateCount_ = 0;
    // Timestamps are held as doubles of seconds since baseNs_, not as
    // absolute nanoseconds, so a long session cannot erode the mantissa:
    // the loop's own arithmetic works on differences far smaller than the
    // absolute time, and rebasing keeps them that way.
    std::int64_t baseNs_ = 0;

    // t0_ is the filtered time of the most recent callback, t1_ the
    // prediction for the next, e2_ the filtered block period. t0_ pairs
    // with samples_ - an exact integer - which is what makes the mapping
    // below a filtered time against an unfiltered sample count rather than
    // two estimates multiplied together.
    double t0_ = 0.0;
    double t1_ = 0.0;
    double e2_ = 0.0;
    std::int64_t samples_ = 0;

    double estimatedSampleRate_ = 0.0;
};

}  // namespace jamn::engine
