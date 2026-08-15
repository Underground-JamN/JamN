#include "jamn_bench/bench_result.h"

#include <cstddef>
#include <cstdio>
#include <sstream>

namespace jamn::bench {

namespace {

// AudioBlockTiming's name fields are fixed buffers, filled from a device
// start callback where nothing should allocate. Its writer always
// terminates them, but reading one as a bare const char* would run off the
// end if that ever stopped being true, so the size is the bound.
template <std::size_t N>
std::string FromFixedBuffer(const char (&buffer)[N]) {
    std::size_t length = 0;
    while (length < N && buffer[length] != '\0') ++length;
    return std::string(buffer, length);
}

std::string EscapeJsonString(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (unsigned char c : value) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    escaped += buffer;
                } else {
                    escaped += static_cast<char>(c);
                }
        }
    }
    return escaped;
}

}  // namespace

std::string ToJson(const std::vector<BenchResult>& results) {
    std::ostringstream out;
    out << "{\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const BenchResult& r = results[i];
        out << "    {\n";
        out << "      \"backend\": \"" << EscapeJsonString(r.backend) << "\",\n";
        out << "      \"device\": \"" << EscapeJsonString(r.device) << "\",\n";
        out << "      \"sample_rate_hz\": " << r.sample_rate_hz << ",\n";
        out << "      \"block_size\": " << r.block_size << ",\n";
        out << "      \"num_blocks\": " << r.num_blocks << ",\n";
        out << "      \"callback_duration_ns\": {\n";
        out << "        \"min\": " << r.callback_duration.min_ns << ",\n";
        out << "        \"max\": " << r.callback_duration.max_ns << ",\n";
        out << "        \"mean\": " << r.callback_duration.mean_ns << "\n";
        out << "      },\n";
        out << "      \"xruns\": " << r.xruns << ",\n";
        out << "      \"events_per_second\": " << r.events_per_second << ",\n";
        out << "      \"clock_offset_p99_us\": " << r.clock_offset_p99_us << ",\n";
        out << "      \"callback_interval_us\": {\n";
        out << "        \"min\": " << r.callback_interval.min_us << ",\n";
        out << "        \"max\": " << r.callback_interval.max_us << ",\n";
        out << "        \"mean\": " << r.callback_interval.mean_us << ",\n";
        out << "        \"p50\": " << r.callback_interval.p50_us << ",\n";
        out << "        \"p99\": " << r.callback_interval.p99_us << "\n";
        out << "      },\n";
        out << "      \"device_starts\": " << r.device_starts << ",\n";
        out << "      \"frames\": " << r.frames << ",\n";
        out << "      \"span_us\": " << r.span_us << ",\n";
        out << "      \"delivered_rate_hz\": " << r.delivered_rate_hz << ",\n";
        out << "      \"configured_buffer_depth_samples\": " << r.configured_buffer_depth_samples << ",\n";
        out << "      \"notes\": \"" << EscapeJsonString(r.notes) << "\"\n";
        out << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

BenchResult FromAudioBlockTiming(const jamn::core::AudioBlockTiming& timing) {
    BenchResult result;
    // The device type, not the word "juce": on Windows the distinction that
    // matters (WASAPI shared vs exclusive) is a type, and the schema's
    // backend field is an open string precisely so it can carry one. Only
    // a device that never started leaves it empty.
    const std::string type = FromFixedBuffer(timing.typeName);
    result.backend = type.empty() ? "audio-device" : type;
    result.device = FromFixedBuffer(timing.deviceName);

    // As granted, not as requested - a device is free to refuse a block
    // size or a rate and keep its own, and a row that reported the request
    // would silently describe a run that never happened.
    result.sample_rate_hz = timing.sampleRate > 0.0 ? static_cast<int>(timing.sampleRate) : -1;
    result.block_size = timing.blockSize > 0 ? timing.blockSize : -1;
    // Not guarded to -1: zero callbacks entered is a real and quite
    // interesting reading, not a missing one.
    result.num_blocks = static_cast<int>(timing.blocks);
    result.frames = timing.blocks > 0 ? timing.frames : -1;
    result.device_starts = timing.deviceStarts > 0 ? static_cast<std::int64_t>(timing.deviceStarts) : -1;
    result.span_us = timing.spanNs > 0 ? timing.spanNs / 1000 : -1;
    result.configured_buffer_depth_samples = timing.outputLatencySamples;
    // Set here rather than by changing the struct's defaults, which the
    // in-process backends rely on and which would change the shape of every
    // bench.json already captured. A device measures arrival, not cost, so
    // 0.0 would be a claim this path cannot make.
    result.callback_duration.min_ns = -1.0;
    result.callback_duration.max_ns = -1.0;
    result.callback_duration.mean_ns = -1.0;

    if (timing.intervalCount > 0) {
        result.callback_interval.min_us = timing.minIntervalUs;
        result.callback_interval.max_us = timing.maxIntervalUs;
        result.callback_interval.mean_us = timing.meanIntervalUs;
        result.callback_interval.p50_us = timing.p50IntervalUs;
        result.callback_interval.p99_us = timing.p99IntervalUs;
    }

    // Frames per second of wall clock. Over blocks-1 because the span
    // measures from the *first* callback's entry, so the frames that
    // callback delivered fall outside it - worth about one interval of
    // slop at the boundary, which is why a small difference from the
    // device's reported rate is noise rather than drift.
    if (timing.blocks > 1 && timing.spanNs > 0) {
        result.delivered_rate_hz = static_cast<double>(timing.frames - timing.blockSize) * 1e9 /
                                   static_cast<double>(timing.spanNs);
    }

    return result;
}

}  // namespace jamn::bench
