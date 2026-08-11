#pragma once

#include <string>
#include <vector>

namespace jamn::bench {

struct CallbackDurationStats {
    double min_ns = 0.0;
    double max_ns = 0.0;
    double mean_ns = 0.0;
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
};

std::string ToJson(const std::vector<BenchResult>& results);

}  // namespace jamn::bench
