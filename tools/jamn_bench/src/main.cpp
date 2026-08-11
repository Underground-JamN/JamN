// jamn_bench: measures callback timing over a real or simulated audio
// path and reports it as the JSON schema in bench_result.h. Only the
// FileAudioDevice backend exists so far - jamn_platform doesn't have a
// real ALSA/WASAPI backend yet, so there is nothing here yet that measures
// actual xruns, device latency or Windows timer granularity. Do not read
// this backend's numbers as hardware performance data.

#include "jamn_bench/bench_result.h"
#include "jamn_core/file_audio_device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

using jamn::bench::BenchResult;
using jamn::bench::ToJson;
using jamn::core::FileAudioDevice;

namespace {

BenchResult RunFileAudioDeviceBench(int sampleRateHz, int blockSize, int numBlocks) {
    FileAudioDevice device(2, blockSize);

    std::vector<double> durationsNs;
    // Reserved up front so the timed loop below never reallocates, even
    // though this tool isn't linked with jamn_core's allocation trap -
    // the point is to measure callback cost, not push_back's.
    durationsNs.reserve(static_cast<std::size_t>(numBlocks));

    device.Process(numBlocks, [&](float* const* output, int numChannels, int numFrames) {
        const auto start = std::chrono::steady_clock::now();
        for (int channel = 0; channel < numChannels; ++channel) {
            for (int frame = 0; frame < numFrames; ++frame) {
                output[channel][frame] = 0.0f;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        durationsNs.push_back(std::chrono::duration<double, std::nano>(end - start).count());
    });

    BenchResult result;
    result.backend = "file-audio-device";
    result.device = "in-process, no real hardware";
    result.sample_rate_hz = sampleRateHz;
    result.block_size = blockSize;
    result.num_blocks = numBlocks;
    result.xruns = -1;  // not applicable: no real hardware buffer to underrun
    result.notes =
        "FileAudioDevice runs unpaced (as fast as the CPU allows), so this "
        "measures callback CPU cost only, not real xrun/latency behavior. "
        "The callback itself is a placeholder zero-fill, not real DSP work.";

    if (!durationsNs.empty()) {
        double sum = 0.0;
        double minValue = std::numeric_limits<double>::max();
        double maxValue = 0.0;
        for (double duration : durationsNs) {
            sum += duration;
            minValue = std::min(minValue, duration);
            maxValue = std::max(maxValue, duration);
        }
        result.callback_duration.min_ns = minValue;
        result.callback_duration.max_ns = maxValue;
        result.callback_duration.mean_ns = sum / static_cast<double>(durationsNs.size());
    }

    return result;
}

}  // namespace

int main() {
    std::vector<BenchResult> results;
    results.push_back(RunFileAudioDeviceBench(48000, 128, 512));

    std::printf("%s", ToJson(results).c_str());
    return 0;
}
