#pragma once

#include <cstdint>

namespace jamn::core {

// What an audio device's callback actually saw: the (cumulative samples,
// steady-clock time at callback entry) pair AudioClock will be fed from, plus
// enough interval statistics to tell a plausible reading from an implausible
// one. Nothing here is derived from the device's reported sample rate - the
// whole point of Clock 2 is that the device's own crystal and the CPU's are
// not the same crystal, so a rate asserted by the driver cannot be used to
// check a rate measured at the callback.
//
// Lives in jamn_core, not in the JUCE-linking module that fills it, so that
// consumers which must not see JUCE can still read a reading: jamn_bench_lib
// converts one of these into a BenchResult, and that conversion is unit-
// tested under `ctest -L fast` in the core-only preset, where no device and
// no JUCE exist. It is a plain POD with no device types in it, so the split
// costs nothing.
struct AudioBlockTiming {
    std::uint64_t blocks = 0;  // Callbacks entered.
    // Device starts seen. More than one means the sample timeline was
    // broken mid-run: `frames` is still the run's true total, but it is
    // not one continuous count, and AudioClock would have been
    // re-prepared at each break. Reported rather than folded away
    // because Phase 0's xrun acceptance is read off this.
    std::uint64_t deviceStarts = 0;
    // What the device said it would run at, recorded at the most
    // recent start. Carried in here rather than left to the caller to
    // read off sampleRate()/blockSize(), because Close() zeroes those
    // and Close() is exactly what has to happen before this struct can
    // be read at all - so a caller doing the obvious thing gets zeros.
    //
    // They are also what make the interval figures below mean
    // anything: whether a 10879us median is healthy depends entirely
    // on whether nominal is 512/48000 (10667us) or 512/44100
    // (11610us), and those two readings are otherwise indistinguishable
    // from a mean alone.
    double sampleRate = 0.0;
    int blockSize = 0;
    // Which device produced this reading. A fixed buffer rather than a
    // std::string because it is filled from the device's start callback,
    // and nothing reached from there should allocate even where it is
    // currently allowed to.
    char deviceName[128] = {};
    // The device *type* that produced it - "ALSA", "JACK", "WASAPI",
    // "CoreAudio". Distinct from deviceName, and not derivable from it:
    // on Linux the sound server's default and a raw hw: card are both
    // "ALSA" and only deviceName tells them apart, while on Windows the
    // interesting distinction (WASAPI shared vs exclusive) runs the other
    // way. A bench row needs both. Same fixed-buffer reasoning as above.
    char typeName[32] = {};
    // **Not a measured latency, and must not be reported as one.**
    // JUCE's ALSA backend computes this as period_size * (periods - 1)
    // - its own source calls it "the method JACK uses to guess the
    // latency" - so it is the configured buffer depth and nothing
    // else. It cannot see USB transfer time, the DAC's pipeline, or a
    // sound server's own buffering: on the dev box a raw hw: card and
    // the PipeWire default both reported exactly 384 samples, which
    // they self-evidently do not both cost.
    //
    // Kept because buffer depth is worth knowing and is what a caller
    // would otherwise infer wrongly from block size alone. Phase 0's
    // latency acceptance needs a real round-trip measurement, not
    // this. -1 where the device would not say.
    int outputLatencySamples = -1;
    std::int64_t frames = 0;       // Total frames delivered, all starts.
    std::int64_t lastEntryNs = 0;  // steady_clock at the last callback entry.
    std::int64_t spanNs = 0;       // Last callback entry minus the first.
    // How many intervals actually went into the figures below. The
    // measured/not-measured signal for all five, and not derivable from
    // `blocks`: a device restart deliberately drops the interval spanning
    // the gap, so a two-block run across a restart has two blocks and no
    // intervals at all. Without this a consumer has to guess, and 0us is a
    // plausible-looking wrong guess.
    std::int64_t intervalCount = 0;
    std::int64_t minIntervalUs = 0;
    std::int64_t maxIntervalUs = 0;
    std::int64_t meanIntervalUs = 0;
    // Histogram64 is a 5-second sliding window, so these two describe the
    // end of the run rather than all of it, and report bucket upper
    // bounds - the same caveat NetThread::Cadence carries.
    std::int64_t p50IntervalUs = 0;
    std::int64_t p99IntervalUs = 0;
};

}  // namespace jamn::core
