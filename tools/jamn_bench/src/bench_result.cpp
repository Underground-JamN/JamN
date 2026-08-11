#include "jamn_bench/bench_result.h"

#include <cstdio>
#include <sstream>

namespace jamn::bench {

namespace {

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
        out << "      \"notes\": \"" << EscapeJsonString(r.notes) << "\"\n";
        out << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

}  // namespace jamn::bench
