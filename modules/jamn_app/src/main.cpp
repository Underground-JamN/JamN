#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#include "jamn_bench/bench_result.h"
#include "jamn_core/file_audio_device.h"
#include "jamn_core/session_limits.h"
#include "jamn_dsp/jam_audio.h"
#include "jamn_dsp/test_tone_instrument.h"
#include "jamn_engine/audio_runtime.h"
#include "jamn_engine/event_scheduler.h"
#include "jamn_engine/net_thread.h"
#include "jamn_engine/peer_runtime.h"
#include "jamn_net/enet_transport.h"
#include "jamn_platform/juce_audio_device.h"
#include "jamn_ui/jam_window_content.h"

namespace {

const char* FindOption(int argc, char* argv[], const char* name) {
    for (int index = 1; index < argc - 1; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return nullptr;
}

std::int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Same clock and same epoch as NowUs, in the finer unit AudioClock's input
// side wants - see JuceAudioDevice's SteadyNs for why nanoseconds there.
std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// How a session is reached, parsed once in main() and read by both the
// headless path and the GUI one, so neither re-parses argv.
//
// `--listen <port>` binds 127.0.0.1 only. A host that other machines can
// reach is a widening (EnetTransport::Listen already takes the flag for
// it) that belongs with the join-by-ip:port dialog, not here: opening a
// listening socket on every interface is a decision worth making once,
// visibly, alongside the UI that makes it useful.
struct NetOptions {
    enum class Role { kNone, kListen, kConnect };

    Role role = Role::kNone;
    std::string host;
    std::uint16_t port = 0;
    // Headless only: how long the net session runs before it is stopped and
    // scored. Ignored by the GUI, which runs until the window closes.
    std::int64_t runMs = 3000;
};

// Device controls. holdMs is headless-only - the GUI runs until its window
// closes - but the rest apply to both: --device decides which card either
// path opens, and --bench-json is wanted most on the GUI path, since that
// is the only reading taken from a device driven for as long as somebody
// actually played.
struct DeviceOptions {
    // How long to hold the device open before closing it and reporting.
    // 250ms is enough for a few hundred callbacks at any sane block size
    // and keeps jamn_app_smoke a smoke test; Phase 0's "zero xruns over 5
    // minutes" acceptance is --device-ms 300000.
    std::int64_t holdMs = 250;
    // 0 means "device's choice", which is what it was until Phase 0's
    // acceptance turned out to name 128 frames specifically.
    int blockSize = 0;
    double sampleRate = 0.0;
    // Empty means the system default. --list-devices prints what is
    // accepted here; the strings are JUCE's own and are not ALSA "hw:X,Y"
    // spellings, so they cannot be guessed.
    std::string name;
    // Where to write this run's reading as a jamn_bench row. Empty means
    // don't - the reading is still printed either way, and every number in
    // the printed form is reachable from the JSON one. Exists because the
    // real-device numbers this binary takes were otherwise trapped in
    // terminal output while jamn_bench, which owns the JSON schema, cannot
    // open a device at all.
    std::string benchJsonPath;
};

DeviceOptions ParseDeviceOptions(int argc, char* argv[]) {
    DeviceOptions out;
    if (const char* holdMs = FindOption(argc, argv, "--device-ms")) out.holdMs = std::atoll(holdMs);
    if (const char* block = FindOption(argc, argv, "--block-size")) out.blockSize = std::atoi(block);
    if (const char* rate = FindOption(argc, argv, "--sample-rate")) out.sampleRate = std::atof(rate);
    if (const char* name = FindOption(argc, argv, "--device")) out.name = name;
    if (const char* path = FindOption(argc, argv, "--bench-json")) out.benchJsonPath = path;
    return out;
}

// Parsed in main() alongside g_netOptions, for the same reason: JUCE
// constructs the application through a function pointer taking no
// arguments. holdMs is ignored by the GUI, which runs until its window
// closes - but --device is not, and matters most there: two instances on
// one box sharing one set of speakers cannot be told apart by ear, and
// pointing them at different cards is what fixes that.
DeviceOptions g_deviceOptions;

bool ParsePort(const char* text, std::uint16_t& out) {
    const long value = std::strtol(text, nullptr, 10);
    if (value <= 0 || value > 65535) return false;
    out = static_cast<std::uint16_t>(value);
    return true;
}

// Accepts host:port as one argument, which is the form the join dialog will
// eventually take a string in - so the CLI and the UI ask for the same
// thing rather than the CLI inventing a second spelling.
bool ParseHostPort(const char* text, std::string& host, std::uint16_t& port) {
    const char* colon = std::strrchr(text, ':');
    if (colon == nullptr || colon == text) return false;
    host.assign(text, static_cast<std::size_t>(colon - text));
    return ParsePort(colon + 1, port);
}

// Returns false only on a malformed argument, having already said which.
bool ParseNetOptions(int argc, char* argv[], NetOptions& out) {
    if (const char* port = FindOption(argc, argv, "--listen")) {
        if (!ParsePort(port, out.port)) {
            std::fprintf(stderr, "jamn_app: --listen wants a port in 1..65535, got '%s'\n", port);
            return false;
        }
        out.role = NetOptions::Role::kListen;
    }
    if (const char* address = FindOption(argc, argv, "--connect")) {
        if (out.role != NetOptions::Role::kNone) {
            std::fprintf(stderr, "jamn_app: --listen and --connect are mutually exclusive\n");
            return false;
        }
        if (!ParseHostPort(address, out.host, out.port)) {
            std::fprintf(stderr, "jamn_app: --connect wants host:port, got '%s'\n", address);
            return false;
        }
        out.role = NetOptions::Role::kConnect;
    }
    if (const char* runMs = FindOption(argc, argv, "--net-ms")) {
        out.runMs = std::atoll(runMs);
    }
    return true;
}

// Brings the transport up for whichever role was asked for. Separate from
// the runtime's own construction because a failure here is an ordinary
// outcome (a port in use, a host that isn't listening yet), not a crash.
bool OpenTransport(jamn::net::EnetTransport& transport, const NetOptions& options) {
    if (options.role == NetOptions::Role::kListen) {
        if (transport.Listen(options.port, /*maxPeers=*/jamn::engine::PeerRuntime::kMaxPeers)) return true;
        std::fprintf(stderr, "jamn_app: could not bind 127.0.0.1:%u\n", options.port);
        return false;
    }
    if (transport.Connect(options.host.c_str(), options.port)) return true;
    std::fprintf(stderr, "jamn_app: could not start connect to %s:%u\n", options.host.c_str(), options.port);
    return false;
}

void PrintCadence(const jamn::engine::NetThread& net) {
    jamn::engine::NetThread::Cadence cadence;
    if (!net.TakeCadence(cadence)) {
        std::printf("jamn_app: net thread cadence unavailable (still running)\n");
        return;
    }
    // The achieved interval, not the requested one - the two differ, and
    // the clock-accuracy budget is spent against the achieved one.
    std::printf(
        "jamn_app: net thread cadence: requested=%lldus achieved mean=%lldus min=%lldus max=%lldus "
        "p50=%lldus p99=%lldus over %llu iterations\n",
        static_cast<long long>(cadence.requestedUs), static_cast<long long>(cadence.meanUs),
        static_cast<long long>(cadence.minUs), static_cast<long long>(cadence.maxUs),
        static_cast<long long>(cadence.p50Us), static_cast<long long>(cadence.p99Us),
        static_cast<unsigned long long>(cadence.iterations));
}

// Set from a signal handler, so nothing here may be anything richer than
// a lock-free integral flag.
std::atomic<bool> g_interrupted{false};

void HandleInterrupt(int) { g_interrupted.store(true, std::memory_order_relaxed); }

// Holds the device open for holdMs, reporting progress, and returns early
// on Ctrl-C so an interrupted soak still produces its numbers instead of
// nothing.
void HoldDeviceOpen(std::int64_t holdMs) {
    constexpr std::int64_t kSliceMs = 250;
    // Often enough to show a long run is alive, rare enough not to bury
    // the reading that follows it.
    constexpr std::int64_t kProgressEveryMs = 15'000;

    std::signal(SIGINT, HandleInterrupt);
    std::signal(SIGTERM, HandleInterrupt);

    if (holdMs >= kProgressEveryMs) {
        std::printf("jamn_app --headless: holding the device open for %llds (Ctrl-C stops early and still reports)\n",
                    static_cast<long long>(holdMs / 1000));
        std::fflush(stdout);
    }

    std::int64_t elapsedMs = 0;
    std::int64_t nextProgressMs = kProgressEveryMs;
    while (elapsedMs < holdMs && !g_interrupted.load(std::memory_order_relaxed)) {
        const std::int64_t slice = std::min<std::int64_t>(kSliceMs, holdMs - elapsedMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        elapsedMs += slice;
        if (holdMs >= kProgressEveryMs && elapsedMs >= nextProgressMs) {
            std::printf("jamn_app --headless: %llds of %llds\n", static_cast<long long>(elapsedMs / 1000),
                        static_cast<long long>(holdMs / 1000));
            std::fflush(stdout);
            nextProgressMs += kProgressEveryMs;
        }
    }

    if (g_interrupted.load(std::memory_order_relaxed)) {
        std::printf("jamn_app --headless: interrupted after %llds - reporting what was measured\n",
                    static_cast<long long>(elapsedMs / 1000));
        std::fflush(stdout);
    }

    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
}

// What the audio thread's half of the session actually did. Exists because
// the audible test cannot distinguish the two processes when both play to
// one set of speakers: clicking on the host makes a sound either way, so
// "did the note cross" needs an answer that is not a sound. Read after the
// device is closed, for the same reason PrintBlockTiming is - these
// counters are written by the audio thread with no synchronisation.
void PrintAudioSessionStats(const jamn::engine::AudioRuntime& session) {
    const jamn::engine::AudioRuntime::Stats& stats = session.stats();
    std::printf("jamn_app: audio session: scheduled=%llu delivered=%llu stale=%llu before_lock=%llu "
                "rejected=%llu relocks=%llu departed=%llu flushed=%llu capped_blocks=%llu\n",
                static_cast<unsigned long long>(stats.notesScheduled),
                static_cast<unsigned long long>(stats.notesDelivered),
                static_cast<unsigned long long>(stats.notesFromStaleSlot),
                static_cast<unsigned long long>(stats.notesBeforeClockLock),
                static_cast<unsigned long long>(stats.notesRejectedByScheduler),
                static_cast<unsigned long long>(stats.reLocksSeen),
                static_cast<unsigned long long>(stats.peersDeparted),
                static_cast<unsigned long long>(stats.notesFlushedOnReLock + stats.notesFlushedOnPeerLoss),
                static_cast<unsigned long long>(stats.blocksAtNoteCapacity));
    // Locked or not is printed first, because the estimate is meaningless
    // without it and looks perfectly reasonable either way. The loop needs
    // about 6.4 seconds (four time constants at 0.1Hz) before it means
    // anything, and a short session simply never gets there - measured on
    // the dev box, runs of 2-5 seconds scattered over +-680ppm while
    // 26-second runs of the same device landed inside +-42ppm.
    const bool locked = session.clock().IsLocked();
    std::printf("jamn_app: audio session: clock 2 %s after %zu blocks%s\n",
                locked ? "locked" : "STILL CONVERGING", session.clock().UpdateCount(),
                locked ? "" : " - the estimate below is not yet meaningful");
    std::printf("jamn_app: audio session: device sample rate estimate %.2fHz (%+.0f ppm from nominal)\n",
                session.clock().EstimatedSampleRate(), session.clock().DriftPpm());
    std::fflush(stdout);
}

// T5.1's reading, and the only place it is visible: the sample count and
// the callback-entry times the audio thread actually saw. It is printed
// rather than asserted because no machine an agent can reach has an audio
// device - /dev/snd is absent inside a sandboxed session, so this path is
// verified by a maintainer at a real machine.
//
// What a good reading looks like: deviceStarts=1, blocks rising with the
// hold below, frames == blocks * the device's block size, and mean
// interval == blockSize/sampleRate. A mean far off that, or a max several
// times the mean, is the device missing deadlines.
void PrintBlockTiming(const jamn::platform::JuceAudioDevice& device) {
    jamn::platform::JuceAudioDevice::BlockTiming timing;
    if (!device.TakeBlockTiming(timing)) {
        // Either no device was ever opened - the ordinary outcome on a
        // machine with no sound card - or one is still open, which would
        // make the read a data race. Neither is a measurement.
        std::printf("jamn_app: audio block timing: no measurement (device never opened, or still open)\n");
        return;
    }
    if (timing.blocks == 0) {
        std::printf("jamn_app: audio block timing: device opened but entered no callbacks\n");
        return;
    }
    // Nominal is printed next to the measurements because without it they
    // cannot be judged, and the two candidate readings are easy to
    // confuse: 512 frames at 48kHz is 10667us, at 44.1kHz it is 11610us,
    // and "mean sits between them" reads as either a healthy 44.1k device
    // or a 48k device dropping one callback in eleven.
    const std::int64_t nominalUs =
        (timing.sampleRate > 0.0 && timing.blockSize > 0)
            ? static_cast<std::int64_t>(1e6 * timing.blockSize / timing.sampleRate)
            : 0;
    std::printf(
        "jamn_app: audio block timing: device='%s' rate=%.0fHz blockSize=%d nominal=%lldus blocks=%llu "
        "frames=%lld starts=%llu span=%lldus\n",
        timing.deviceName, timing.sampleRate, timing.blockSize, static_cast<long long>(nominalUs),
        static_cast<unsigned long long>(timing.blocks), static_cast<long long>(timing.frames),
        static_cast<unsigned long long>(timing.deviceStarts), static_cast<long long>(timing.spanNs / 1000));
    if (timing.outputLatencySamples >= 0 && timing.sampleRate > 0.0) {
        std::printf("jamn_app: audio block timing: configured buffer depth %d samples (%.2f ms) - NOT measured "
                    "latency, see below\n",
                    timing.outputLatencySamples, 1000.0 * timing.outputLatencySamples / timing.sampleRate);
    } else {
        std::printf("jamn_app: audio block timing: output latency not reported by this device\n");
    }
    std::printf(
        "jamn_app: audio block timing: interval mean=%lldus min=%lldus max=%lldus p50=%lldus p99=%lldus\n",
        static_cast<long long>(timing.meanIntervalUs), static_cast<long long>(timing.minIntervalUs),
        static_cast<long long>(timing.maxIntervalUs), static_cast<long long>(timing.p50IntervalUs),
        static_cast<long long>(timing.p99IntervalUs));
    // **Intervals at twice nominal are not evidence of a dropped callback**,
    // which is the trap this line exists to keep a reader out of. On a
    // resampling server they are the expected cadence: PipeWire running a
    // 48kHz/512 graph for a stream opened at 44100 has nothing to hand
    // this callback on 1 - 44100/48000 = 8.1% of its cycles, so 8.1% of
    // intervals double and the mean still lands on nominal
    // (10667 / 0.91875 = 11609us, against 11610us nominal - measured on
    // the dev box, not derived on paper).
    //
    // What does catch a real loss is the delivered rate: frames actually
    // handed over per second of wall clock. Callbacks that never happened
    // are frames that never arrived, so this falls below the device's
    // reported rate; the interval statistics alone cannot tell the two
    // apart. Computed over blocks-1 because span measures from the first
    // callback's entry, and worth about one interval of slop at the
    // boundary - 1400ppm over ten seconds, so treat a small difference as
    // measurement noise rather than drift. Clock 2 is what measures drift
    // properly, over thousands of callbacks rather than two.
    if (timing.spanNs > 0 && timing.blocks > 1 && timing.sampleRate > 0.0) {
        const double deliveredRate =
            static_cast<double>(timing.frames - timing.blockSize) * 1e9 / static_cast<double>(timing.spanNs);
        std::printf("jamn_app: audio block timing: delivered %.0fHz against a reported %.0fHz (%+.0f ppm; "
                    "materially below means samples were lost)\n",
                    deliveredRate, timing.sampleRate, (deliveredRate / timing.sampleRate - 1.0) * 1e6);
    }
    // Printed from shutdown() on the way out, so flushed explicitly rather
    // than left to however the process ends.
    std::fflush(stdout);
}

// Returns empty if --bench-json's path can be written, or a reason it
// cannot. Checked before the device is opened, not after: the write itself
// happens at the very end of a run that may have been a five-minute soak,
// and finding out then that the path was a typo throws the whole
// measurement away. That is the same wart HoldDeviceOpen's Ctrl-C handling
// exists to avoid, and it costs one open to rule out.
//
// Probes in append mode so an existing file's contents survive being
// checked, and removes the file again if the probe is what created it -
// leaving an empty stub behind would be its own kind of fabricated
// measurement.
std::string CheckBenchJsonPathWritable(const std::string& path) {
    std::error_code ignored;
    const bool existed = std::filesystem::exists(path, ignored);
    {
        std::ofstream probe(path, std::ios::app);
        if (!probe) return "could not open '" + path + "' for writing";
    }
    if (!existed) std::filesystem::remove(path, ignored);
    return {};
}

// The same reading PrintBlockTiming just printed, as a jamn_bench row.
// Returns false only when the file could not be written - a run with no
// measurement to report is not a failure, it is the ordinary outcome on a
// machine with no sound card.
//
// Must be called after Close() and never before: TakeBlockTiming refuses
// while the device is open, because the counters it reads are written by
// the audio thread with no synchronisation. Both call sites sit
// immediately after the PrintBlockTiming that already has to obey this,
// and TakeBlockTiming is a pure read, so calling it a second time costs
// nothing and changes nothing.
bool WriteBenchJson(const jamn::platform::JuceAudioDevice& device, const std::string& path, bool interrupted) {
    jamn::platform::JuceAudioDevice::BlockTiming timing;
    if (!device.TakeBlockTiming(timing)) {
        // Deliberately writes no file. A row of -1s is indistinguishable
        // from a real reading of an unmeasurable device once it is sitting
        // in a JSON file with no context, and a fabricated measurement is
        // worse than a missing one.
        std::printf("jamn_app: --bench-json: no measurement to write (device never opened, or still open) - "
                    "no file written\n");
        std::fflush(stdout);
        return true;
    }

    jamn::bench::BenchResult result = jamn::bench::FromAudioBlockTiming(timing);
    // What the row cannot carry in a field, and what a reader diffing two
    // of them has to know: p50/p99 come from a 5-second sliding window, so
    // comparing a five-second run's tail against a five-minute run's tail
    // is comparing five seconds against five seconds either way - and a
    // soak that was cut short otherwise looks exactly like one that ran to
    // completion.
    result.notes = "Real audio device driven through JUCE. callback_duration is not measured on this path - "
                   "the device reports when callbacks arrived, not what they cost. xruns is not counted; do "
                   "not infer it from the interval spread, which a resampling sound server widens on its own. "
                   "callback_interval p50/p99 come from a 5-second sliding window and describe the end of the "
                   "run, not all of it, and are bucket upper bounds - so either may read above max, which is "
                   "exact. Compare them against each other across runs, not against min/max within one.";
    if (interrupted) {
        result.notes += " This run was interrupted (Ctrl-C) and is shorter than it was asked to be.";
    }
    if (result.device_starts > 1) {
        result.notes += " device_starts is above 1: the sample timeline broke mid-run, so frames is a true "
                        "total but not one continuous count. Re-take this reading rather than interpreting it.";
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::fprintf(stderr, "jamn_app: --bench-json: could not open '%s' for writing\n", path.c_str());
        return false;
    }
    file << jamn::bench::ToJson({result});
    // Checked rather than assumed: a full disk fails here, not at open.
    file.close();
    if (!file) {
        std::fprintf(stderr, "jamn_app: --bench-json: could not write '%s'\n", path.c_str());
        return false;
    }
    std::printf("jamn_app: --bench-json: wrote %s\n", path.c_str());
    std::fflush(stdout);
    return true;
}

// Prints every output device JUCE can see, then exits. Exists because the
// name --device takes is JUCE's own and not an ALSA "hw:0,0" - listing it
// is the only way to know what to pass.
int RunListDevices() {
    // Same reason RunHeadless needs it: AudioDeviceManager reaches into
    // MIDI enumeration and AsyncUpdater, both of which assert a
    // MessageManager exists, even though nothing here opens a window.
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    jamn::platform::JuceAudioDevice device;
    const std::vector<jamn::platform::JuceAudioDevice::DeviceEntry> entries = device.ListOutputDevices();
    if (entries.empty()) {
        std::printf("jamn_app: no output devices found\n");
        return 0;
    }
    std::printf("jamn_app: output devices (pass one to --device, quoted exactly as shown):\n");
    std::string lastType;
    for (const jamn::platform::JuceAudioDevice::DeviceEntry& entry : entries) {
        if (entry.type != lastType) {
            std::printf("  [%s]%s\n", entry.type.c_str(), entry.isCurrentType ? "  (current type)" : "");
            lastType = entry.type;
        }
        std::printf("    %s\n", entry.name.c_str());
    }
    std::fflush(stdout);
    return 0;
}

// The headless net self-check: bring a real socket up, run the net thread
// against it for a bounded time, play notes at one end and count what
// arrives at the other, then stop and score it.
//
// The notes are generated here rather than by any shipped input path
// because there is no input path yet (Wave 6). What is under test is the
// thread and its shutdown, not the notes - but a run that exchanged nothing
// would look identical to a run that worked, so something has to play.
//
// This thread stands in for the audio thread, driving the same
// AudioRuntime the GUI path drives from its callback - one consumer, which
// is the single-consumer half of NoteCrossing's SPSC contract. There is no
// device here, so the "blocks" are this loop's iterations.
// The transport arrives already open: a host has to be bound before a
// client can reach it, and binding it here would put the audio-device check
// in front of that. On this dev box the device check returns "no channels"
// in milliseconds; on a machine with real cards it does not, and a driver
// script's head start would then be timing something invisible to it.
int RunHeadlessNetSession(jamn::net::EnetTransport& transport, const NetOptions& options) {
    constexpr std::int64_t kNoteIntervalUs = 25'000;
    constexpr std::int64_t kNoteHoldUs = 12'000;
    // Enough silence at the end for trailing K=3 burst copies and the last
    // note-off to land before anything is counted.
    constexpr std::int64_t kDrainUs = 1'000'000;
    // How long to wait for a peer before giving up and reporting that none
    // arrived. ENet retransmits its connect command, so this is generous
    // rather than tight.
    constexpr std::int64_t kConnectWaitUs = 15'000'000;

    const std::int64_t startUs = NowUs();
    jamn::engine::AudioRuntime audioSide;
    audioSide.Prepare(48'000.0, 128);
    // Declared in this order so the thread is joined before the runtime it
    // borrows, which is before the transport the runtime borrows. Stop() is
    // still called explicitly below - relying on declaration order alone
    // would make the ordering invisible at the point it matters.
    jamn::engine::PeerRuntime runtime(transport, startUs);
    jamn::engine::NetThread net(runtime);

    // Atomic because this is the one piece of state the two threads share:
    // the callback fires inside Service, on the net thread, and the loop
    // below reads it from this one. A plain bool here would be a data race
    // that neither sanitizer preset could catch, since both are core-only
    // scope and this file links JUCE.
    std::atomic<bool> everConnected{false};
    runtime.SetPeerEventCallback([&everConnected](jamn::net::PeerId, jamn::net::PeerEvent event) {
        if (event == jamn::net::PeerEvent::kConnected) everConnected.store(true, std::memory_order_relaxed);
    });

    net.Start();

    std::set<std::pair<jamn::net::PeerId, std::uint8_t>> held;
    std::uint64_t submitted = 0;
    std::uint64_t delivered = 0;
    std::uint64_t noteOns = 0;
    std::uint64_t noteOffs = 0;

    // The play window runs from the moment the link came up, not from
    // process start. The two peers do not start together - a host has to be
    // bound before a client can reach it, and each opens an audio device
    // first at whatever speed its machine manages - so windows anchored to
    // process start end at different times, and whichever peer stops first
    // stops before the other's last note-off has been sent. That is a stuck
    // note measuring the harness rather than the runtime. Connection is the
    // one instant both peers observe together.
    std::int64_t playUntilUs = 0;
    std::int64_t stopUs = 0;
    std::int64_t nextNoteOnUs = 0;
    bool haveHeldNote = false;
    std::int64_t heldUntilUs = 0;
    std::uint8_t heldNote = 0;
    std::uint8_t noteCounter = 0;

    // Bounds a run where the peer never turns up at all: without it, a
    // client pointed at nothing would sit in this loop forever waiting for
    // a window that never opens.
    const std::int64_t connectDeadlineUs = startUs + kConnectWaitUs;

    while (true) {
        const std::int64_t nowUs = NowUs();
        const bool connected = everConnected.load(std::memory_order_relaxed);
        // Opening the window comes before testing it. The other order stops
        // the run on the first connected iteration, since the deadline it
        // would be tested against has not been set yet.
        if (connected && playUntilUs == 0) {
            playUntilUs = nowUs + options.runMs * 1000;
            stopUs = playUntilUs + kDrainUs;
            nextNoteOnUs = nowUs;
        }
        if (!connected && nowUs >= connectDeadlineUs) break;
        if (connected && nowUs >= stopUs) break;

        if (connected) {
            if (nowUs < playUntilUs && nowUs >= nextNoteOnUs && !haveHeldNote) {
                jamn::proto::NoteEvent on;
                on.kind = jamn::proto::NoteEventKind::kNoteOn;
                on.a = noteCounter++ % 128;
                if (runtime.SubmitLocalEvent(on, nowUs)) ++submitted;
                haveHeldNote = true;
                heldNote = on.a;
                heldUntilUs = nowUs + kNoteHoldUs;
                nextNoteOnUs = nowUs + kNoteIntervalUs;
            }
            // Not gated on the play window: a note still held when that
            // window closes has to be released anyway, or the receiver's
            // stuck note measures this loop rather than the runtime.
            if (haveHeldNote && nowUs >= heldUntilUs) {
                jamn::proto::NoteEvent off;
                off.kind = jamn::proto::NoteEventKind::kNoteOff;
                off.a = heldNote;
                if (runtime.SubmitLocalEvent(off, nowUs)) ++submitted;
                haveHeldNote = false;
            }
        }

        // Stands in for the audio thread, and drives the real AudioRuntime
        // rather than reaching into the crossing directly - so what this
        // scores is the whole audio-side path (drain, timebase conversion,
        // jitter buffer, scheduler) and not just the ring. Frames are
        // derived from elapsed time at a nominal 48kHz: there is no device
        // here, and Clock 2 needs a plausible sample count more than an
        // exact one, since nothing below asserts on its estimate.
        const std::int64_t frames = (nowUs - startUs) * 48;
        std::array<jamn::engine::AudioRuntime::Note, jamn::engine::AudioRuntime::kMaxNotesPerBlock> notes;
        const std::size_t count = audioSide.Service(&runtime, frames, NowNs(), notes.data(), notes.size());
        for (std::size_t index = 0; index < count; ++index) {
            const jamn::engine::AudioRuntime::Note& note = notes[index];
            const auto key = std::make_pair(note.peer, note.event.a);
            if (note.event.kind == jamn::proto::NoteEventKind::kNoteOn) {
                ++delivered;
                ++noteOns;
                held.insert(key);
            } else if (note.event.kind == jamn::proto::NoteEventKind::kNoteOff) {
                ++delivered;
                ++noteOffs;
                held.erase(key);
            } else if (note.event.kind == jamn::proto::NoteEventKind::kAllNotesOff) {
                // The peer left, or its clock re-locked - either way its
                // queue was discarded, so anything it still held is
                // released rather than counted as stuck. Not a delivered
                // note: nobody played it, so counting it would make
                // delivered exceed submitted.
                for (auto it = held.begin(); it != held.end();) {
                    it = (it->first == note.peer) ? held.erase(it) : std::next(it);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    net.Stop();

    const jamn::engine::PeerRuntime::Stats& stats = runtime.stats();
    std::printf("jamn_app: net session role=%s connected=%d submitted=%llu delivered=%llu on=%llu off=%llu "
                "stuck=%zu deduped=%llu clock_samples=%llu\n",
                options.role == NetOptions::Role::kListen ? "listen" : "connect",
                everConnected.load(std::memory_order_relaxed) ? 1 : 0,
                static_cast<unsigned long long>(submitted), static_cast<unsigned long long>(delivered),
                static_cast<unsigned long long>(noteOns), static_cast<unsigned long long>(noteOffs), held.size(),
                static_cast<unsigned long long>(stats.notesDeduped),
                static_cast<unsigned long long>(stats.clockSamplesFolded));
    PrintCadence(net);
    std::fflush(stdout);

    // Each of these is a way this run could exit 0 having proven nothing.
    if (!everConnected.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "jamn_app: net session never saw a connected peer\n");
        return 3;
    }
    if (delivered == 0 || noteOns == 0 || noteOffs == 0) {
        std::fprintf(stderr, "jamn_app: net session connected but exchanged no notes\n");
        return 4;
    }
    if (!held.empty()) {
        std::fprintf(stderr, "jamn_app: net session left %zu stuck note(s)\n", held.size());
        return 5;
    }
    if (stats.packetsRejected != 0 || stats.packetsFromUnknownPeer != 0 || stats.notesDroppedAtCrossing != 0 ||
        stats.sendsFailed != 0 || stats.packetsTooLarge != 0 || stats.localEventsDropped != 0) {
        std::fprintf(stderr, "jamn_app: net session runtime reported a fault\n");
        return 6;
    }
    return 0;
}

// Two deterministic checks, neither timing-dependent:
//  (a) can the platform backend be opened and closed without crashing?
//      Passes whether or not this machine has a sound card - which outcome
//      is not the assertion.
//  (b) does the shipped signal path run? A fixed block count through the
//      JUCE-free fake device, so it's deterministic regardless of (a).
//      Never run a fixed block count on the real device - that would be
//      timing-dependent and flaky.
//
// A third check runs only when --listen or --connect was given: the net
// session above. Without one of those flags this path behaves exactly as it
// did before, which is what keeps jamn_app_smoke a device check rather than
// a network one.
int RunHeadless(int argc, char* argv[], const NetOptions& netOptions) {
    // AudioDeviceManager reaches into MIDI device enumeration and
    // AsyncUpdater internally, both of which assert that a MessageManager
    // exists and that the calling thread is the message thread - true even
    // though this path never opens a window. This is JUCE's own documented
    // fix for exactly this: "particularly handy... at the beginning of a
    // console app's main()".
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    // Before anything else, so a listening socket is bound within
    // milliseconds of process start rather than after however long this
    // machine takes to open and close an audio device. Declared first, so
    // it also outlives everything the net session builds on top of it.
    jamn::net::EnetTransport transport;
    if (netOptions.role != NetOptions::Role::kNone && !OpenTransport(transport, netOptions)) return 2;

    const DeviceOptions& deviceOptions = g_deviceOptions;
    // Before the device, and before any hold - see CheckBenchJsonPathWritable.
    if (!deviceOptions.benchJsonPath.empty()) {
        const std::string pathError = CheckBenchJsonPathWritable(deviceOptions.benchJsonPath);
        if (!pathError.empty()) {
            std::fprintf(stderr, "jamn_app: --bench-json: %s\n", pathError.c_str());
            return 3;
        }
    }

    // Declared in this order so the device is destroyed before audio, same
    // as JamnApplication below - the audio thread must never touch a dead
    // JamAudio.
    jamn::dsp::JamAudio audio;
    jamn::platform::JuceAudioDevice device;

    const std::string error =
        device.Open(2, [&audio](double sampleRate, int) { audio.Prepare(sampleRate); },
                    [&audio](float* const* out, int numChannels, int numFrames) {
                        audio.Process(out, numChannels, numFrames);
                    },
                    deviceOptions.blockSize, deviceOptions.sampleRate, deviceOptions.name);
    std::printf("jamn_app --headless: audio device: %s\n",
                error.empty() ? device.deviceName().c_str() : error.c_str());
    if (error.empty()) {
        // Without a hold the device opened and closed inside a millisecond
        // and the timing reading below had nothing in it.
        //
        // Sliced rather than one long sleep, for two reasons that only
        // showed up once somebody ran a real soak: a five-minute run that
        // prints nothing looks indistinguishable from a hang, and Ctrl-C
        // during one threw away the entire measurement. Phase 0's xrun
        // acceptance is five minutes and criterion #5's soak is sixty, so
        // "interrupting loses everything" is not a small wart.
        HoldDeviceOpen(deviceOptions.holdMs);
    }
    device.Close();
    PrintBlockTiming(device);
    // Failing to produce a file the caller explicitly asked for is a
    // failure of the run, so it is reported as one - but only after the
    // reading has been printed, so an unwritable path never costs the
    // measurement itself.
    bool benchJsonOk = true;
    if (!deviceOptions.benchJsonPath.empty()) {
        benchJsonOk = WriteBenchJson(device, deviceOptions.benchJsonPath,
                                     g_interrupted.load(std::memory_order_relaxed));
    }

    audio.Prepare(48000.0);
    audio.SetGain(0.5f);
    audio.Trigger();

    jamn::core::FileAudioDevice fake(2, 128);
    if (const char* path = FindOption(argc, argv, "--out")) {
        fake.SetOutputPath(path);
    }
    fake.Process(256, [&audio](float* const* out, int numChannels, int numFrames) {
        audio.Process(out, numChannels, numFrames);
    });

    std::printf("jamn_app --headless: rendered 256 blocks of 128 frames\n");

    if (netOptions.role != NetOptions::Role::kNone) {
        const int netResult = RunHeadlessNetSession(transport, netOptions);
        // A net-session failure is the more specific finding, so it wins.
        return netResult != 0 ? netResult : (benchJsonOk ? 0 : 3);
    }
    return benchJsonOk ? 0 : 3;
}

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(const juce::String& name, std::unique_ptr<juce::Component> content)
        : DocumentWindow(name,
                          juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                              juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(content.release(), true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

// Parsed once in main(), before any JUCE machinery starts. JUCE constructs
// the application object itself through a function pointer taking no
// arguments, so the options have to reach it through something main() can
// fill in first - and its own getCommandLineParameterArray() would mean
// parsing argv twice, in two spellings that could drift apart.
NetOptions g_netOptions;


class JamnApplication final : public juce::JUCEApplication {
public:
    JamnApplication() : netOptions_(g_netOptions) {}

    const juce::String getApplicationName() override { return "JamN"; }
    const juce::String getApplicationVersion() override { return JAMN_VERSION_STRING; }

    // True, and it has to be. Running a host and a client on one box is
    // not a developer convenience here - it is how criterion #1's clock
    // reading is taken and how a remote note is heard at all before a
    // second machine is involved. This was JUCE's template default of
    // false until 2026-08-15, which made the second process print
    // "Another instance is running" and quit before opening anything.
    // Nothing here is single-instance anyway: each process binds its own
    // port, and sharing the audio device is the OS's problem, not ours.
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override {
        // Said now rather than at shutdown, where it would be found only
        // after a whole session had been played and would take that
        // session's reading with it. Not fatal, on the same stance as a
        // missing sound card: the window still opens and the controls
        // still work - the player just gets a chance to fix the path and
        // restart before playing anything.
        if (!g_deviceOptions.benchJsonPath.empty()) {
            const std::string pathError = CheckBenchJsonPathWritable(g_deviceOptions.benchJsonPath);
            if (!pathError.empty()) {
                std::fprintf(stderr, "jamn_app: --bench-json: %s - this run's reading will not be saved\n",
                             pathError.c_str());
            }
        }

        // Both lambdas below run on the message thread and nowhere else -
        // that is the single-producer half of JamAudio's trigger ring's
        // SpscRing contract.
        // One instrument per strip, assigned once and never reassigned. A
        // slot's peer changes over a session but its instrument need not:
        // what has to be cleaned up when a peer goes is the sounding
        // notes, and AudioRuntime already emits an all-notes-off for that.
        for (std::size_t slot = 0; slot < jamn::core::kMaxPeers; ++slot) {
            audio_.peers().strip(slot).SetInstrument(&peerInstruments_[slot]);
        }
        // Outside the mixer, so no peer's mute or solo can silence the
        // player to themselves - see JamAudio::SetLocalInstrument.
        audio_.SetLocalInstrument(&localInstrument_);

        const std::string error = device_.Open(
            2,
            [this](double sampleRate, int blockSize) {
                audio_.Prepare(sampleRate);
                for (auto& instrument : peerInstruments_) instrument.Prepare(sampleRate);
                localInstrument_.Prepare(sampleRate);
                audioSession_.Prepare(sampleRate, blockSize);
                audioFrames_ = 0;
            },
            [this](float* const* out, int numChannels, int numFrames) {
                DispatchNotes(numFrames);
                audio_.Process(out, numChannels, numFrames);
            },
            g_deviceOptions.blockSize, g_deviceOptions.sampleRate, g_deviceOptions.name);

        if (!error.empty()) {
            // No sound card is not a fatal condition - the window still
            // opens and the controls still work, they just make no noise.
            // This dev box is exactly that machine.
            DBG("JamN: audio device unavailable: " << error);
            audio_.Prepare(48000.0);
        }

        auto content = std::make_unique<jamn::ui::JamWindowContent>();
        content->onButtonClicked = [this] {
            audio_.Trigger();
            PlayNote();
        };
        content->onGainChanged = [this](float gain) { audio_.SetGain(gain); };
        // Both go to the same entry point the computer keyboard will, so
        // a mouse-played note is indistinguishable from a typed one by
        // the time it reaches the ring.
        content->onNotePressed = [this](std::uint8_t pitch) {
            SubmitLocalNote(jamn::proto::NoteEventKind::kNoteOn, pitch);
        };
        content->onNoteReleased = [this](std::uint8_t pitch) {
            SubmitLocalNote(jamn::proto::NoteEventKind::kNoteOff, pitch);
        };
        // Local only, and deliberately: panic silences this machine, it
        // does not reach across the wire and mute anyone else's playing.
        content->onPanicClicked = [this] { audioSession_.RequestPanic(); };
        // The slider is constructed with dontSendNotification (see
        // jam_window_content.cpp), so onGainChanged never fires on its
        // own for the initial value - without this, MasterBus would sit
        // at its own default (unity) while the slider displays kDefaultGain.
        audio_.SetGain(jamn::ui::JamWindowContent::kDefaultGain);
        window_ = std::make_unique<MainWindow>(getApplicationName(), std::move(content));

        StartNetSession();
    }

    void shutdown() override {
        // Reverse of the order these came up in, and it is not merely
        // tidiness. Three participants borrow downward - net_ borrows
        // runtime_, which borrows transport_, and since T5.3 the **audio
        // thread borrows runtime_ too**, through DispatchNotes. So
        // both threads have to be stopped before runtime_ is destroyed,
        // and each stop is a join in its own way: NetThread::Stop() joins
        // the net thread, and Close() cannot return while an audio
        // callback is in flight (JUCE takes the same lock the callback
        // holds).
        //
        // device_.Close() sat *below* runtime_.reset() until 2026-08-15,
        // which was correct right up until T5.3 gave the audio callback a
        // reason to touch runtime_ - after which it was a use-after-free
        // that a null check happened to mask most of the time.
        net_.reset();
        device_.Close();
        runtime_.reset();
        transport_.reset();
        // After Close(), never before - both reads are only safe once no
        // callback can be in flight. This is the one reading that comes
        // from a real GUI session rather than --headless's hold, so it is
        // the one that reflects a device driven for as long as somebody
        // actually played.
        PrintBlockTiming(device_);
        // Here as well as in --headless, and this is the more valuable of
        // the two: a GUI run's reading comes from a device driven for as
        // long as somebody actually played, rather than a fixed hold.
        // g_interrupted is never set on this path - a window close is not
        // an interruption - so the run is always reported as complete.
        if (!g_deviceOptions.benchJsonPath.empty() &&
            !WriteBenchJson(device_, g_deviceOptions.benchJsonPath, /*interrupted=*/false)) {
            setApplicationReturnValue(3);
        }
        PrintAudioSessionStats(audioSession_);
        window_.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    // Fixed, because T6.1 has no velocity: nothing that reaches
    // SubmitLocalNote can express one yet - a computer key is down or it
    // is not - so inventing a curve here would be a number with no input
    // behind it.
    static constexpr std::uint8_t kLocalVelocity = 100;

    // **The one message-thread entry point every local input source
    // funnels through**, and the reason the ring below it needs no merge
    // step: AudioRuntime's local-monitoring ring is a single-producer
    // SpscRing, and "single producer" holds because every source arrives
    // here first. The button is the only source today; T6.1's keyboard
    // capture and mouse strip join it here rather than beside it.
    void SubmitLocalNote(jamn::proto::NoteEventKind kind, std::uint8_t pitch) {
        jamn::proto::NoteEvent event;
        event.kind = kind;
        event.a = pitch;
        // A note-off leaves velocity at zero, the way this path always
        // did - the field means nothing for one, and the wire bytes stay
        // what they were.
        if (kind == jamn::proto::NoteEventKind::kNoteOn) event.b = kLocalVelocity;

        // Monitoring first, and not for the microseconds: it is the half
        // that has to happen whether or not there is a session at all. A
        // player running solo still hears themselves.
        audioSession_.SubmitLocalNote(event);

        if (jamn::engine::PeerRuntime* runtime = runtime_.get(); runtime != nullptr) {
            runtime->SubmitLocalEvent(event, NowUs());
        }
    }

    // Plays one note locally and sends it to every peer. Still a button
    // and still not T6.1's real input path - that is per-OS raw scan codes
    // with 6-key rollover - but it now goes through the same entry point
    // that path will, so what it exercises is the real thing rather than a
    // stand-in.
    //
    // The local blip still sounds through BlipVoice as it always did,
    // separately: that one proves the device, this one proves the note.
    void PlayNote() {
        // Walks a pentatonic rather than repeating one pitch, so several
        // clicks are audibly distinct - "did the note arrive" is a much
        // harder question to answer when every note is the same one, and
        // "land musically" is the actual acceptance being served.
        static constexpr std::uint8_t kScale[] = {60, 62, 64, 67, 69, 72};
        const std::uint8_t pitch = kScale[nextNote_ % (sizeof(kScale) / sizeof(kScale[0]))];
        ++nextNote_;

        SubmitLocalNote(jamn::proto::NoteEventKind::kNoteOn, pitch);

        // The note-off goes out on its own, later, from the message thread
        // - the only thread allowed to submit. Sending it now with a future
        // timestamp would be the obvious shortcut and the wrong one: the
        // burst assembler would ship both in the same burst and the
        // receiver would schedule a note that is already over.
        juce::Timer::callAfterDelay(
            250, [this, pitch] { SubmitLocalNote(jamn::proto::NoteEventKind::kNoteOff, pitch); });
    }

    // The audio thread, at block start, and the entire jamn_app half of
    // T5.3 and T6.1. Everything with a decision in it - draining the
    // crossing and the local ring, converting out of the sender's
    // timebase, scheduling, the re-lock flush - is AudioRuntime's, in a
    // JUCE-free module that `ctest -L fast` and both sanitizer presets can
    // see. What is left here is turning a NoteEvent into an instrument
    // call, because only this module links both jamn_engine and jamn_dsp.
    //
    // **Runs with or without a session.** It returned early on a null
    // runtime until T6.1, which was right while every note was somebody
    // else's; local monitoring is not, so the early return would now make
    // a solo player silent. AudioRuntime::Service takes the null.
    //
    // The `runtime_` read is formally a race: initialise() opens the
    // device before StartNetSession() assigns it, so this thread reads a
    // pointer the message thread writes once, later. Neither sanitizer
    // preset can see it - both are core-only scope, and this file is not
    // in it - so it is written down here rather than left to be
    // rediscovered. Benign in practice on every platform this targets (an
    // aligned pointer word), and the fix if it ever stops being benign is
    // an atomic in this class, not a lock on the audio thread.
    void DispatchNotes(int numFrames) {
        jamn::engine::PeerRuntime* runtime = runtime_.get();

        // Its own frame counter and clock read rather than
        // JuceAudioDevice's: the device's pair is a measurement taken for
        // reporting, and reaching into another object's audio-thread state
        // from inside its callback would be safe only by an argument about
        // call ordering. Both are one add and one clock read.
        const std::int64_t steadyNs = NowNs();
        const std::int64_t blockFirstFrame = audioFrames_;
        audioFrames_ += numFrames;

        std::array<jamn::engine::AudioRuntime::Note, jamn::engine::AudioRuntime::kMaxNotesPerBlock> notes;
        const std::size_t count =
            audioSession_.Service(runtime, blockFirstFrame, steadyNs, notes.data(), notes.size());

        for (std::size_t index = 0; index < count; ++index) {
            const jamn::engine::AudioRuntime::Note& note = notes[index];

            // **Slot first, peer id second**, and the order matters.
            // Local input owns no slot, so a note that has one belongs to
            // a strip; a note without one is the local monitor's if its
            // peer says so, and skipped otherwise (a remote note whose
            // peer had already left its slot). Testing the peer id first
            // would misroute panic's all-notes-off for an *empty* slot,
            // because PeerRuntime::kNoPeer and kLocalPeerId are the same
            // 0xFFFF - the slot is the unambiguous half of the pair.
            jamn::dsp::IInstrument* instrument = nullptr;
            if (note.slot < jamn::core::kMaxPeers) {
                instrument = audio_.peers().strip(note.slot).instrument();
            } else if (note.peer == jamn::engine::EventScheduler::kLocalPeerId) {
                instrument = &localInstrument_;
            }
            if (instrument == nullptr) continue;

            switch (note.event.kind) {
                case jamn::proto::NoteEventKind::kNoteOn:
                    instrument->NoteOn(note.event.a, note.event.b);
                    break;
                case jamn::proto::NoteEventKind::kNoteOff:
                    instrument->NoteOff(note.event.a);
                    break;
                case jamn::proto::NoteEventKind::kAllNotesOff:
                    instrument->AllNotesOff();
                    break;
            }
        }
    }

    void StartNetSession() {
        if (netOptions_.role == NetOptions::Role::kNone) return;

        transport_ = std::make_unique<jamn::net::EnetTransport>();
        if (!OpenTransport(*transport_, netOptions_)) {
            // Same stance as a missing sound card: the window still opens
            // and the local controls still work, they just have nobody to
            // play with.
            transport_.reset();
            return;
        }
        runtime_ = std::make_unique<jamn::engine::PeerRuntime>(*transport_, NowUs());
        net_ = std::make_unique<jamn::engine::NetThread>(*runtime_);
        net_->Start();
        // The audio thread drains runtime_->crossing() from
        // DispatchNotes above, which is the only consumer - the
        // single-consumer half of NoteCrossing's SPSC contract.
    }

    // Declaration order is load-bearing twice over: device_ is destroyed
    // before audio_, so the audio thread can never touch a dead JamAudio;
    // and net_ is destroyed before runtime_ and transport_, so the net
    // thread can never touch either of those dead. shutdown() does both
    // explicitly - this is the backstop, not the mechanism.
    NetOptions netOptions_;
    jamn::dsp::JamAudio audio_;
    // One per strip, so their lifetime is the application's - a strip
    // holds a borrowed pointer and must never outlive what it points at.
    // Declared before device_ for the same reason audio_ is: the audio
    // thread must never reach a destroyed instrument.
    std::array<jamn::dsp::TestToneInstrument, jamn::core::kMaxPeers> peerInstruments_;
    // The player's own monitoring. Same type as a peer's for now, and
    // owned here for the same lifetime reason - JamAudio holds a borrowed
    // pointer to it and the audio thread must never reach it destroyed.
    jamn::dsp::TestToneInstrument localInstrument_;
    jamn::engine::AudioRuntime audioSession_;
    // Audio thread only, reset at every device start.
    std::int64_t audioFrames_ = 0;
    // Message thread only - which button clicks are.
    std::size_t nextNote_ = 0;
    jamn::platform::JuceAudioDevice device_;
    std::unique_ptr<jamn::net::EnetTransport> transport_;
    std::unique_ptr<jamn::engine::PeerRuntime> runtime_;
    std::unique_ptr<jamn::engine::NetThread> net_;
    std::unique_ptr<MainWindow> window_;
};

juce::JUCEApplicationBase* CreateJamnApplication() {
    return new JamnApplication();
}

}  // namespace

// What START_JUCE_APPLICATION expands to on this platform (see
// juce_Initialisation.h), spelled out so the --headless check below can run
// before any JUCE GUI machinery starts - a contributor's machine may have
// no display, and --headless must still work there.
int main(int argc, char* argv[]) {
    if (!ParseNetOptions(argc, argv, g_netOptions)) return 2;
    g_deviceOptions = ParseDeviceOptions(argc, argv);

    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--list-devices") == 0) {
            return RunListDevices();
        }
        if (std::strcmp(argv[index], "--headless") == 0) {
            return RunHeadless(argc, argv, g_netOptions);
        }
    }

    juce::JUCEApplicationBase::createInstance = &CreateJamnApplication;
    return juce::JUCEApplicationBase::main(argc, const_cast<const char**>(argv));
}
