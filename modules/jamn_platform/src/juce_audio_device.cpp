#include "jamn_platform/juce_audio_device.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "jamn_core/realtime_scope.h"

namespace jamn::platform {
namespace {

// The same reading NowUs() takes in jamn_app - steady_clock's own epoch,
// not a session-relative one - so a timestamp captured here and a local
// time resolved through ClockSync are on one timebase. Nanoseconds rather
// than microseconds because this is Clock 2's raw input: at 128 frames and
// 48kHz a block is 2667us, and microsecond rounding would inject about
// 190ppm of quantisation noise per callback into a rate estimate whose
// whole job is to resolve a drift measured in ppm.
std::int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

JuceAudioDevice::JuceAudioDevice() = default;

JuceAudioDevice::~JuceAudioDevice() {
    Close();
}

std::vector<JuceAudioDevice::DeviceEntry> JuceAudioDevice::ListOutputDevices() {
    std::vector<DeviceEntry> entries;
    // Populates the type list on first call, which is why this is not
    // const - enumerating devices is a scan, not a read.
    const juce::OwnedArray<juce::AudioIODeviceType>& types = deviceManager_.getAvailableDeviceTypes();
    const juce::String currentType = deviceManager_.getCurrentAudioDeviceType();

    for (juce::AudioIODeviceType* type : types) {
        if (type == nullptr) continue;
        type->scanForDevices();  // Required before getDeviceNames says anything.
        const juce::StringArray names = type->getDeviceNames(/*wantInputNames=*/false);
        for (const juce::String& name : names) {
            entries.push_back(DeviceEntry{type->getTypeName().toStdString(), name.toStdString(),
                                          type->getTypeName() == currentType});
        }
    }
    return entries;
}

std::string JuceAudioDevice::Open(int numOutputChannels,
                                   PrepareCallback prepare,
                                   jamn::core::AudioCallback callback,
                                   int requestedBlockSize,
                                   double requestedSampleRate,
                                   const std::string& requestedDeviceName) {
    // std::function assignment allocates - confined to Open, never the RT
    // callback, which only ever invokes callback_, and invoking one doesn't
    // allocate.
    prepare_ = std::move(prepare);
    callback_ = std::move(callback);

    // One Open() is one measurement. Reopening after a Close() starts a
    // fresh one rather than pooling two runs - the mirror of NetThread
    // refusing to restart at all, which it can afford to do because a net
    // thread is never legitimately restarted and a device is.
    measured_ = false;
    blocks_ = 0;
    deviceStarts_ = 0;
    startedRate_ = 0.0;
    startedBlockSize_ = 0;
    startedDeviceName_.fill('\0');
    startedTypeName_.fill('\0');
    startedOutputLatency_ = -1;
    frames_ = 0;
    firstEntryNs_ = 0;
    lastEntryNs_ = 0;
    previousEntryNs_ = 0;
    havePreviousEntry_ = false;
    intervalCount_ = 0;
    intervalSumUs_ = 0;
    intervalMinUs_ = 0;
    intervalMaxUs_ = 0;
    intervals_ = jamn::core::Histogram64{};

    // A named device may belong to a type that is not the current one, and
    // initialise() will not go looking across types for it - so the type
    // is selected first, from the same enumeration --list-devices prints.
    if (!requestedDeviceName.empty()) {
        for (const DeviceEntry& entry : ListOutputDevices()) {
            if (entry.name == requestedDeviceName && !entry.isCurrentType) {
                deviceManager_.setCurrentAudioDeviceType(juce::String(entry.type), true);
                break;
            }
        }
    }

    // An explicit setup rather than initialise()'s preferredDefaultDeviceName,
    // because that path searches for a name matching an input *and* an
    // output and sets both when it finds one. A raw ALSA `hw:` card with a
    // capture side - which both USB cards on the dev box have - therefore
    // gets opened for capture as well, and if anything else holds that
    // capture stream the whole open fails with "no channels", naming no
    // culprit. This asks for an output and nothing else.
    juce::AudioDeviceManager::AudioDeviceSetup setup = deviceManager_.getAudioDeviceSetup();
    setup.inputDeviceName = {};
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    if (!requestedDeviceName.empty()) setup.outputDeviceName = juce::String(requestedDeviceName);
    if (requestedBlockSize > 0) setup.bufferSize = requestedBlockSize;
    if (requestedSampleRate > 0.0) setup.sampleRate = requestedSampleRate;

    const bool haveRequest = !requestedDeviceName.empty() || requestedBlockSize > 0 || requestedSampleRate > 0.0;
    const juce::String error =
        deviceManager_.initialise(0, numOutputChannels, nullptr, true, juce::String(), haveRequest ? &setup : nullptr);
    if (error.isNotEmpty()) {
        std::string detail = error.toStdString() + " (requested output '" +
                             (requestedDeviceName.empty() ? std::string("<default>") : requestedDeviceName) + "'";
        // ALSA reports "no channels" both when the name matched nothing -
        // JUCE looks it up by exact string and leaves the device id empty
        // on a miss - and when the name matched but no output channel
        // survived. Those need completely different fixes, and the message
        // alone cannot tell them apart, so say which happened.
        if (!requestedDeviceName.empty()) {
            bool nameFound = false;
            for (const DeviceEntry& entry : ListOutputDevices()) {
                if (entry.name == requestedDeviceName) {
                    nameFound = true;
                    break;
                }
            }
            detail += nameFound ? "; the name matched an enumerated device, so the open itself failed"
                                : "; that name is NOT in the enumerated output list - check --list-devices";
        }
        detail += ")";
        return detail;
    }

    // initialise() falls back to the default rather than failing when a
    // named device cannot be opened - which is the right behaviour for an
    // app but the wrong silence for a measurement, since every reading is
    // meant to name the device it came from. Say so and carry on.
    if (!requestedDeviceName.empty()) {
        const juce::AudioIODevice* chosen = deviceManager_.getCurrentAudioDevice();
        if (chosen == nullptr || chosen->getName().toStdString() != requestedDeviceName) {
            std::fprintf(stderr, "jamn_platform: '%s' was not available; using '%s' instead\n",
                         requestedDeviceName.c_str(), chosen != nullptr ? chosen->getName().toRawUTF8() : "(none)");
        }
    }

    // initialise() can return no error and still leave no device selected -
    // this machine has no sound card at all. Trusting the empty error
    // string alone would hand a null device to audioDeviceAboutToStart.
    if (deviceManager_.getCurrentAudioDevice() == nullptr) {
        return "no audio output device is available on this system";
    }

    // Re-applied after initialise, because initialise may have opened the
    // device with its own defaults where the requested figures were not
    // achievable together. Return value deliberately ignored: a device
    // that will not run at 128 frames keeps its own size and stays
    // perfectly usable, and the reading reports what was granted. Failing
    // Open here would turn "this card cannot do 128" into "the app will
    // not start". Before addAudioCallback, so no callback can be in
    // flight while the device restarts underneath it.
    if (requestedBlockSize > 0 || requestedSampleRate > 0.0) {
        juce::AudioDeviceManager::AudioDeviceSetup granted = deviceManager_.getAudioDeviceSetup();
        if (requestedBlockSize > 0) granted.bufferSize = requestedBlockSize;
        if (requestedSampleRate > 0.0) granted.sampleRate = requestedSampleRate;
        deviceManager_.setAudioDeviceSetup(granted, true);
    }

    deviceManager_.addAudioCallback(this);
    open_ = true;
    measured_ = true;
    return {};
}

void JuceAudioDevice::Close() {
    if (!open_) {
        return;
    }
    // Remove the callback before closing the device, so no callback can be
    // in flight while this object's members are torn down.
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
    open_ = false;
}

std::string JuceAudioDevice::deviceName() const {
    if (const juce::AudioIODevice* device = deviceManager_.getCurrentAudioDevice()) {
        return device->getName().toStdString();
    }
    return {};
}

void JuceAudioDevice::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                         int numInputChannels,
                                                         float* const* outputChannelData,
                                                         int numOutputChannels,
                                                         int numSamples,
                                                         const juce::AudioIODeviceCallbackContext& context) {
    // hostTimeNs is NOT a hardware timestamp on this platform. JUCE 8.0.9's
    // AudioIODeviceCallbackContext is exactly `{ const uint64_t* hostTimeNs
    // = nullptr; }` and only the CoreAudio backend ever populates it, so
    // steady_clock at callback entry is the primary source here and the
    // context stays unused. docs/CLOCK.md carries the same correction.
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

    // Opened here rather than immediately around callback_ below, so it
    // covers everything this function does on the audio thread - the
    // timing record, the channel filtering and the zero-fill are all
    // real-time code and all belong inside the marker the allocation trap
    // watches.
    jamn::core::RealtimeScope scope;

    // First, before any of the channel work below - a timestamp taken after
    // the block has been prepared measures this function, not the device.
    RecordBlock(numSamples);

    const int usable = std::min(numOutputChannels, kMaxOutputChannels);
    int usableChannels = 0;
    for (int channel = 0; channel < usable; ++channel) {
        if (outputChannelData[channel] != nullptr) {
            channelPointers_[usableChannels++] = outputChannelData[channel];
        }
    }

    // Any channel beyond kMaxOutputChannels still holds undefined data and
    // would play back as noise if left untouched - JUCE's own header warns
    // that unopened/unfiltered channels are not pre-zeroed.
    for (int channel = usable; channel < numOutputChannels; ++channel) {
        if (outputChannelData[channel] != nullptr) {
            std::fill_n(outputChannelData[channel], numSamples, 0.0f);
        }
    }

    callback_(channelPointers_.data(), usableChannels, numSamples);
}

// Arithmetic on members only - no allocation, no lock, no branch on
// anything unbounded - it runs inside the callback's RealtimeScope, so
// the allocation trap covers it. Histogram64::Record is allocation-free
// by construction.
void JuceAudioDevice::RecordBlock(int numSamples) {
    const std::int64_t entryNs = SteadyNs();

    if (blocks_ == 0) {
        firstEntryNs_ = entryNs;
    }
    if (havePreviousEntry_) {
        const std::int64_t intervalUs = (entryNs - previousEntryNs_) / 1000;
        if (intervalCount_ == 0 || intervalUs < intervalMinUs_) intervalMinUs_ = intervalUs;
        if (intervalCount_ == 0 || intervalUs > intervalMaxUs_) intervalMaxUs_ = intervalUs;
        intervalSumUs_ += intervalUs;
        ++intervalCount_;
        intervals_.Record(intervalUs, entryNs / 1000);
    }
    previousEntryNs_ = entryNs;
    havePreviousEntry_ = true;
    lastEntryNs_ = entryNs;

    // The pair Clock 2 consumes is (frames_, entryNs) read here, before the
    // increment: frames_ is the index of this block's first frame, which is
    // what entryNs is the arrival time of. T5.3 wires that; frames_ below
    // is the run's total, one block further along.
    frames_ += numSamples;
    ++blocks_;
}

void JuceAudioDevice::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    const double rate = device->getCurrentSampleRate();
    const int block = device->getCurrentBufferSizeSamples();
    sampleRate_.store(rate, std::memory_order_relaxed);
    blockSize_.store(block, std::memory_order_relaxed);
    ++deviceStarts_;
    // Kept separately from sampleRate_/blockSize_, which audioDeviceStopped
    // zeroes - these have to outlive the device so the reading can be
    // interpreted after Close().
    startedRate_ = rate;
    startedBlockSize_ = block;
    startedOutputLatency_ = device->getOutputLatencyInSamples();
    const std::string name = device->getName().toStdString();
    const std::size_t copied = std::min(name.size(), startedDeviceName_.size() - 1);
    std::memcpy(startedDeviceName_.data(), name.data(), copied);
    startedDeviceName_[copied] = '\0';
    // The device's own type, taken from the device rather than from
    // deviceManager_.getCurrentAudioDeviceType(): this is the object that
    // actually started, so the two cannot disagree.
    const std::string type = device->getTypeName().toStdString();
    const std::size_t typeCopied = std::min(type.size(), startedTypeName_.size() - 1);
    std::memcpy(startedTypeName_.data(), type.data(), typeCopied);
    startedTypeName_[typeCopied] = '\0';
    // A restart is a new sample timeline at a possibly new rate, so the
    // interval spanning the gap is not an interval. Everything else
    // accumulates across the restart, and deviceStarts says it happened.
    havePreviousEntry_ = false;
    if (prepare_) {
        prepare_(rate, block);
    }
}

void JuceAudioDevice::audioDeviceStopped() {
    sampleRate_.store(0.0, std::memory_order_relaxed);
    blockSize_.store(0, std::memory_order_relaxed);
    // Deliberately does not touch the block counters. JUCE calls this from
    // inside removeAudioCallback(), i.e. from Close(), so clearing them
    // here would erase the measurement at the instant TakeBlockTiming
    // becomes allowed to read it.
}

bool JuceAudioDevice::TakeBlockTiming(BlockTiming& out) const {
    if (open_ || !measured_) {
        return false;
    }
    out = BlockTiming{};
    out.blocks = blocks_;
    out.deviceStarts = deviceStarts_;
    out.sampleRate = startedRate_;
    out.blockSize = startedBlockSize_;
    std::memcpy(out.deviceName, startedDeviceName_.data(), sizeof(out.deviceName));
    std::memcpy(out.typeName, startedTypeName_.data(), sizeof(out.typeName));
    out.outputLatencySamples = startedOutputLatency_;
    out.frames = frames_;
    out.lastEntryNs = lastEntryNs_;
    out.spanNs = blocks_ > 0 ? lastEntryNs_ - firstEntryNs_ : 0;
    out.intervalCount = intervalCount_;
    if (intervalCount_ > 0) {
        out.minIntervalUs = intervalMinUs_;
        out.maxIntervalUs = intervalMaxUs_;
        out.meanIntervalUs = intervalSumUs_ / intervalCount_;
        const std::int64_t nowUs = lastEntryNs_ / 1000;
        out.p50IntervalUs = intervals_.Percentile(nowUs, 0.50);
        out.p99IntervalUs = intervals_.P99(nowUs);
    }
    return true;
}

void JuceAudioDevice::audioDeviceError(const juce::String& errorMessage) {
    juce::ignoreUnused(errorMessage);
}

}  // namespace jamn::platform
