#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jamn_core/audio_block_timing.h"

namespace jamn::bench {

struct CallbackDurationStats {
    double min_ns = 0.0;
    double max_ns = 0.0;
    double mean_ns = 0.0;
};

// How far apart successive callbacks *arrived*, which is a different
// measurement from CallbackDurationStats above - that one is how long a
// callback took to run. An in-process backend can measure cost but has no
// meaningful arrival cadence; a real device is the reverse. Keeping them as
// two structs is what stops a reader comparing one against the other.
//
// -1 throughout, not 0, on the same not-measured convention as xruns below:
// a device that entered no callbacks has no intervals, and 0us would read as
// an impossibly fast one.
struct CallbackIntervalStats {
    std::int64_t min_us = -1;
    std::int64_t max_us = -1;
    std::int64_t mean_us = -1;
    // From a 5-second sliding Histogram64 window, so these describe the end
    // of a run rather than all of it, and report bucket upper bounds. It
    // matters when diffing two rows of different lengths - see the notes
    // field, which is where the run's length belongs.
    std::int64_t p50_us = -1;
    std::int64_t p99_us = -1;
};

// backend and device are open strings, not enums, by design: Windows alone
// has more than two backends worth benchmarking later (WASAPI shared,
// WASAPI exclusive, WASAPI low-latency via IAudioClient3, WDM-KS), and this
// schema must not need a migration to add a row for one. Never average
// results across differing device/PCM names together - record each as its
// own row.
struct BenchResult {
    std::string backend;
    std::string device;
    int sample_rate_hz = 0;
    int block_size = 0;
    int num_blocks = 0;
    CallbackDurationStats callback_duration;
    // -1 means "not measured for this backend" (e.g. FileAudioDevice has no
    // real hardware buffer to underrun), not "zero xruns observed".
    int xruns = -1;
    std::string notes;
    // Throughput for backends that measure operations/sec rather than a
    // callback's duration (e.g. event-scheduler). -1 means "not
    // applicable for this backend" - the same not-measured convention
    // xruns uses, not "zero events per second".
    double events_per_second = -1.0;
    // p99 of |estimated peer clock offset| in microseconds, for backends
    // that measure clock accuracy rather than throughput or callback cost.
    // -1 means "not applicable for this backend" - the same not-measured
    // convention xruns and events_per_second use, not "perfectly accurate".
    double clock_offset_p99_us = -1.0;

    // Everything below is filled by a real-device backend and left at -1 by
    // the in-process ones.
    CallbackIntervalStats callback_interval;
    // Device starts seen during the run. **Not decoration.** Anything above
    // 1 means the sample timeline broke mid-run, at which point `frames` is
    // still a true total but is not one continuous count - so a row without
    // this cannot be interpreted at all, and a run with it above 1 should be
    // re-taken rather than reasoned about.
    std::int64_t device_starts = -1;
    std::int64_t frames = -1;
    // First-to-last callback entry. Recorded alongside delivered_rate_hz
    // rather than instead of it: the derivation below is not obvious, and
    // carrying its inputs makes the row self-auditing.
    std::int64_t span_us = -1;
    // Frames actually handed over per second of wall clock, which is what
    // catches a real loss - callbacks that never happened are frames that
    // never arrived. The interval statistics alone cannot: on a resampling
    // sound server, intervals at twice nominal are the expected cadence and
    // not a dropped callback.
    double delivered_rate_hz = -1.0;
    // **The device's configured buffer depth, and deliberately not called a
    // latency.** JUCE's ALSA backend computes it as
    // period_size * (periods - 1), so it cannot see USB transfer, the DAC
    // pipeline, or a sound server's own buffering - on one dev box a raw
    // hw: card and the sound server's default reported an identical 384
    // samples, which they self-evidently do not both cost. A real latency
    // figure needs a round-trip measurement, which nothing here performs.
    int configured_buffer_depth_samples = -1;
};

std::string ToJson(const std::vector<BenchResult>& results);

// The reading a real audio device produced, as a bench row. Lives here, in
// a JUCE-free library, rather than in the app that owns the device, so that
// it is reachable by `ctest -L fast` under the core-only preset - no
// device, no JUCE and no hardware are needed to check the arithmetic.
//
// Fills only what a device measures. callback_duration stays -1: a device
// reports when its callbacks arrived, not what they cost. xruns stays -1
// too, because nothing counts them - and an interval spread must not be
// read as one, since a resampling sound server produces the same shape.
// notes is left to the caller, which is the only party that knows how the
// run ended.
BenchResult FromAudioBlockTiming(const jamn::core::AudioBlockTiming& timing);

}  // namespace jamn::bench
