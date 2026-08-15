// jamn_bench: measures callback timing over a real or simulated audio
// path and reports it as the JSON schema in bench_result.h. Only the
// FileAudioDevice backend exists so far - jamn_platform doesn't have a
// real ALSA/WASAPI backend yet, so there is nothing here yet that measures
// actual xruns, device latency or Windows timer granularity. Do not read
// this backend's numbers as hardware performance data.

#include "jamn_bench/bench_result.h"
#include "jamn_core/file_audio_device.h"
#include "jamn_engine/event_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#ifdef JAMN_BENCH_HAS_ENET
#include <atomic>
#include <cmath>
#include <memory>
#include <thread>

#include "jamn_engine/peer_runtime.h"
#include "jamn_net/enet_transport.h"
#endif

using jamn::bench::BenchResult;
using jamn::bench::ToJson;
using jamn::core::FileAudioDevice;
using jamn::engine::EventScheduler;

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

// Targets the >1M events/s figure ARCHITECTURE_PLAN.md names for
// EventScheduler. Measures the ScheduleLocalEvent + PopReady round trip -
// the zero-added-delay local-input path, and the cheapest one
// EventScheduler has - since that pair is what actually runs per event in
// steady-state operation; pushing without ever popping would measure
// nothing but heap insertion.
BenchResult RunEventSchedulerBench(int numEvents) {
    EventScheduler scheduler;

    jamn::proto::NoteEvent event;
    event.kind = jamn::proto::NoteEventKind::kNoteOn;

    std::size_t delivered = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < numEvents; ++i) {
        event.eventSeq = static_cast<std::uint16_t>(i);
        scheduler.ScheduleLocalEvent(event, /*nowUs=*/0);
        EventScheduler::Delivery delivery;
        if (scheduler.PopReady(/*nowUs=*/0, delivery)) {
            ++delivered;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();

    BenchResult result;
    result.backend = "event-scheduler";
    result.device = "in-process, no real hardware";
    result.num_blocks = numEvents;
    result.events_per_second = seconds > 0.0 ? static_cast<double>(numEvents) / seconds : 0.0;
    result.notes = "ScheduleLocalEvent + PopReady round trip per event, at a fixed nowUs (the "
                    "zero-added-delay local-input path); " +
                    std::to_string(delivered) + "/" + std::to_string(numEvents) + " delivered.";
    return result;
}

#ifdef JAMN_BENCH_HAS_ENET

// Acceptance criterion #1's reading: peer clock offset p99, host and client
// on one box, over real loopback UDP.
//
// **Ground truth is exactly zero, and that is the whole method.** Both
// peers read the same CLOCK_MONOTONIC, so the true offset between them is
// 0us by construction - no reference clock, no calibration, nothing to
// trust. Every microsecond the estimator reports is therefore its own
// error, and p99 of |estimate| is p99 of the error directly.
//
// **What this does not measure**, and must not be read as measuring: a
// genuinely drifting remote oscillator. One box means one crystal, so
// ClockSync's skew and slew paths are exercised at zero drift here. The
// two-machine LAN reading is a separate criterion.
//
// Each peer runs on its own thread with its own socket, rather than both
// being serviced in turn by one loop - otherwise this loop's own latency
// would land inside the measurement as if it were the network's.
struct LoopbackClockPeer {
    jamn::net::EnetTransport transport;
    std::unique_ptr<jamn::engine::PeerRuntime> runtime;
    std::atomic<bool> stop{false};
    std::vector<std::int64_t> offsetSamplesUs;  // Owned by this peer's own thread.
    std::atomic<bool> everLocked{false};
    std::thread thread;
};

// How tightly each peer polls, settable with --poll-interval-us because
// sweeping it is what identified the mechanism below.
//
// A ping waits in the responder's socket until its next poll, and so does
// the pong on the way back; those waits enter the estimate as (d1 - d2) / 2.
// **The waits do not resample.** Both peers poll on a fixed period, so the
// phase between their loops is near-constant and drifts only as slowly as
// the two periods differ - which makes d1 and d2 near-constant too, and the
// resulting error a systematic bias rather than noise. Min-RTT selection
// cannot touch it: every sample carries the same bias *and* the same RTT,
// so there is no less-contaminated sample to select.
//
// Measured here, true offset exactly zero: p99 tracks this interval at
// roughly 1:1 (50us -> 59us, 250us -> 205us, 1000us -> 934us, 4000us ->
// 2772us) rather than being filtered down to the ~0.15x that independent,
// resampled waits would give.
std::int64_t g_pollIntervalUs = 250;

void RunLoopbackClockPeer(LoopbackClockPeer& peer, std::int64_t sampleIntervalUs) {
    std::int64_t nextSampleUs = 0;
    while (!peer.stop.load(std::memory_order_relaxed)) {
        const std::int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count();
        peer.runtime->Service(nowUs);

        if (nowUs >= nextSampleUs) {
            nextSampleUs = nowUs + sampleIntervalUs;
            for (std::size_t slot = 0; slot < jamn::engine::PeerRuntime::kMaxPeers; ++slot) {
                if (peer.runtime->PeerAt(slot) == jamn::engine::PeerRuntime::kNoPeer) continue;
                // Only a locked estimate is a reading. Before lock the
                // published offset is 0 - which here would look like a
                // perfect score rather than an absent one, and would drag
                // the p99 down with samples that measured nothing.
                if (!peer.runtime->OffsetIsLocked(slot)) continue;
                peer.everLocked.store(true, std::memory_order_relaxed);
                peer.offsetSamplesUs.push_back(peer.runtime->PublishedOffsetUs(slot));
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(g_pollIntervalUs));
    }
}

BenchResult RunLoopbackClockBench(int durationSeconds) {
    constexpr std::uint16_t kPortBase = 47950;
    constexpr int kPortAttempts = 40;
    constexpr std::int64_t kSampleIntervalUs = 50'000;  // 20Hz.

    BenchResult result;
    result.backend = "loopback-clock";
    result.device = "two PeerRuntimes, real ENet sockets on 127.0.0.1";
    result.num_blocks = durationSeconds;

    LoopbackClockPeer host;
    LoopbackClockPeer client;

    std::uint16_t port = 0;
    for (int attempt = 0; attempt < kPortAttempts; ++attempt) {
        const std::uint16_t candidate = static_cast<std::uint16_t>(kPortBase + attempt);
        if (host.transport.Listen(candidate, /*maxPeers=*/4)) {
            port = candidate;
            break;
        }
    }
    if (port == 0) {
        result.notes = "could not bind any loopback port; no reading taken";
        return result;
    }
    if (!client.transport.Connect("127.0.0.1", port)) {
        result.notes = "could not start connect to the host; no reading taken";
        return result;
    }

    const std::int64_t startUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
    host.runtime = std::make_unique<jamn::engine::PeerRuntime>(host.transport, startUs);
    client.runtime = std::make_unique<jamn::engine::PeerRuntime>(client.transport, startUs);

    host.thread = std::thread([&host] { RunLoopbackClockPeer(host, kSampleIntervalUs); });
    client.thread = std::thread([&client] { RunLoopbackClockPeer(client, kSampleIntervalUs); });

    std::this_thread::sleep_for(std::chrono::seconds(durationSeconds));
    host.stop.store(true, std::memory_order_relaxed);
    client.stop.store(true, std::memory_order_relaxed);
    host.thread.join();
    client.thread.join();

    // Both directions pooled: each end estimates the other independently,
    // and reporting only the kinder of the two would be picking a winner.
    std::vector<std::int64_t> errorsUs;
    errorsUs.reserve(host.offsetSamplesUs.size() + client.offsetSamplesUs.size());
    for (std::int64_t offsetUs : host.offsetSamplesUs) errorsUs.push_back(std::llabs(offsetUs));
    for (std::int64_t offsetUs : client.offsetSamplesUs) errorsUs.push_back(std::llabs(offsetUs));

    if (errorsUs.empty()) {
        result.notes = host.everLocked.load() || client.everLocked.load()
                            ? "locked but produced no samples; no reading taken"
                            : "clock never locked within the run; no reading taken";
        return result;
    }

    std::sort(errorsUs.begin(), errorsUs.end());
    const std::size_t p99Index =
        std::min(errorsUs.size() - 1, static_cast<std::size_t>(errorsUs.size() * 99 / 100));
    result.clock_offset_p99_us = static_cast<double>(errorsUs[p99Index]);
    result.notes = "Ground truth is exactly 0us (both peers read one machine's steady_clock), so this "
                    "p99 is the estimator's own error over real loopback UDP - " +
                    std::to_string(errorsUs.size()) + " samples, max " +
                    std::to_string(errorsUs.back()) +
                    "us. Does not exercise a drifting remote oscillator: one box, one crystal.";
    return result;
}

#endif  // JAMN_BENCH_HAS_ENET

}  // namespace

int main(int argc, char** argv) {
    std::string backendFilter;  // Empty means "run every backend".
    // Short by default so a routine run stays a few seconds. Criterion #1's
    // actual acceptance reading is 30 wall-clock minutes:
    // --backend loopback-clock --duration-seconds 1800.
    int loopbackClockSeconds = 20;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backendFilter = argv[++i];
        } else if (std::strcmp(argv[i], "--duration-seconds") == 0 && i + 1 < argc) {
            loopbackClockSeconds = std::atoi(argv[++i]);
#ifdef JAMN_BENCH_HAS_ENET
        } else if (std::strcmp(argv[i], "--poll-interval-us") == 0 && i + 1 < argc) {
            g_pollIntervalUs = std::atoll(argv[++i]);
#endif
        }
    }

    std::vector<BenchResult> results;
    if (backendFilter.empty() || backendFilter == "file-audio-device") {
        results.push_back(RunFileAudioDeviceBench(48000, 128, 512));
    }
    if (backendFilter.empty() || backendFilter == "event-scheduler") {
        results.push_back(RunEventSchedulerBench(2'000'000));
    }
    if (backendFilter == "loopback-clock") {
        // Opt-in only, never part of an unfiltered run: it takes wall-clock
        // seconds by construction, and the other two backends are CPU-bound
        // and finish immediately.
#ifdef JAMN_BENCH_HAS_ENET
        results.push_back(RunLoopbackClockBench(loopbackClockSeconds));
#else
        // Explicit rather than silent. Under the core-only preset ENet is
        // never fetched, so this backend does not exist - and an empty
        // results array would read as "measured nothing" instead.
        std::fprintf(stderr,
                      "jamn_bench: the loopback-clock backend needs ENet, which the core-only "
                      "build never fetches. Configure a full preset and re-run.\n");
        return 2;
#endif
    }

    std::printf("%s", ToJson(results).c_str());
    return 0;
}
