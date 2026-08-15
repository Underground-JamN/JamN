#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <juce_audio_devices/juce_audio_devices.h>

#include "jamn_core/audio_block_timing.h"
#include "jamn_core/file_audio_device.h"
#include "jamn_core/histogram64.h"

namespace jamn::platform {

// Opens the system's default audio output through JUCE and drives a
// jamn::core::AudioCallback on the device's real-time thread - the same
// callback shape FileAudioDevice drives in tests, so DSP code moves from one
// to the other unchanged.
class JuceAudioDevice final : private juce::AudioIODeviceCallback {
public:
    // Called once when the device starts or restarts, on the audio thread
    // but with no callback in flight. Not real-time-constrained: this is
    // where Prepare()-style setup (which may call exp/sin) belongs.
    using PrepareCallback = std::function<void(double sampleRate, int blockSize)>;

    // The reading TakeBlockTiming produces. Defined in jamn_core rather
    // than here so that jamn_bench_lib - which must never see JUCE - can
    // convert one into a bench row, and so that conversion can be unit
    // tested in the core-only preset. See jamn_core/audio_block_timing.h
    // for what every field means; the alias keeps the
    // JuceAudioDevice::BlockTiming spelling every caller already uses.
    using BlockTiming = jamn::core::AudioBlockTiming;

    JuceAudioDevice();
    ~JuceAudioDevice() override;

    JuceAudioDevice(const JuceAudioDevice&) = delete;
    JuceAudioDevice& operator=(const JuceAudioDevice&) = delete;

    // Returns an empty string on success, or a human-readable reason on
    // failure. Never throws and never asserts - a machine with no sound
    // card is an ordinary outcome, not a crash.
    // One output device JUCE can see. `type` is JUCE's device-type name
    // (on Linux, "ALSA" and possibly "JACK"), `name` the string Open()
    // takes - which is JUCE's own spelling and not necessarily an ALSA
    // "hw:0,0", so it has to be listed rather than guessed.
    struct DeviceEntry {
        std::string type;
        std::string name;
        bool isCurrentType = false;
    };

    // Every output device, across every type. Message thread, before
    // Open() - this scans hardware and allocates freely.
    std::vector<DeviceEntry> ListOutputDevices();

    // requestedDeviceName of "" means the system default, which is what
    // every caller wanted until Phase 0's latency criterion turned out to
    // need ALSA `default` and `hw:X,Y` measured separately - and until it
    // turned out that two instances on one box sharing one set of speakers
    // cannot be told apart by ear.
    //
    // requestedBlockSize / requestedSampleRate of 0 mean "whatever the
    // device chooses", which is what every caller wanted until Phase 0's
    // acceptance turned out to need a specific figure: "zero xruns over 5
    // minutes at 128 frames" cannot be run against a device that picked
    // 512 on its own. A request the device refuses is not an error - it
    // keeps its own setting, and BlockTiming reports what was actually
    // granted rather than what was asked for, so a refusal is visible in
    // the reading instead of silently assumed to have worked.
    std::string Open(int numOutputChannels, PrepareCallback prepare, jamn::core::AudioCallback callback,
                     int requestedBlockSize = 0, double requestedSampleRate = 0.0,
                     const std::string& requestedDeviceName = {});
    void Close();

    bool isOpen() const noexcept { return open_; }
    std::string deviceName() const;
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    int blockSize() const noexcept { return blockSize_.load(std::memory_order_relaxed); }

    // False, writing nothing, unless a device was actually opened and has
    // since been closed - the same refusal NetThread::TakeCadence makes,
    // for the same reason. The counters below are written by the audio
    // thread with no synchronisation at all, so a live read is a plain
    // data race, and nothing needs one.
    //
    // "Was actually opened" is a separate flag rather than !open_, because
    // Open() can fail - a machine with no sound card is an ordinary
    // outcome here - and a failed Open() leaves open_ false with no
    // callback ever registered. Gating on !open_ alone would report an
    // all-zero reading as a measurement, and a failed *re*-open would
    // report the previous run's counters as this run's.
    //
    // What makes the read safe after Close() is not the open_ flag but
    // JUCE: AudioDeviceManager dispatches the IO callback,
    // audioDeviceAboutToStart and audioDeviceStopped all under one
    // audioCallbackLock, and removeAudioCallback takes that same lock - so
    // Close() cannot return while a callback is in flight, and no callback
    // can be entered after it does. That is this class's equivalent of
    // NetThread::Stop()'s join.
    bool TakeBlockTiming(BlockTiming& out) const;

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

    void RecordBlock(int numSamples);

    static constexpr int kMaxOutputChannels = 32;

    juce::AudioDeviceManager deviceManager_;
    PrepareCallback prepare_;
    jamn::core::AudioCallback callback_;
    std::array<float*, kMaxOutputChannels> channelPointers_{};
    std::atomic<double> sampleRate_{0.0};
    std::atomic<int> blockSize_{0};
    bool open_ = false;
    // Set only by a successful Open(), cleared by the next Open() attempt
    // whether or not it succeeds. TakeBlockTiming's real gate.
    bool measured_ = false;

    // Audio thread only, from Open() until Close() removes the callback.
    // Reset by Open(), never by audioDeviceStopped() - JUCE calls that from
    // inside removeAudioCallback(), so resetting there would empty the
    // counters at the exact moment they become readable.
    std::uint64_t blocks_ = 0;
    std::uint64_t deviceStarts_ = 0;
    double startedRate_ = 0.0;
    int startedBlockSize_ = 0;
    std::array<char, 128> startedDeviceName_{};
    std::array<char, 32> startedTypeName_{};
    int startedOutputLatency_ = -1;
    std::int64_t frames_ = 0;
    std::int64_t firstEntryNs_ = 0;
    std::int64_t lastEntryNs_ = 0;
    // Cleared at every device start, so the interval spanning a restart is
    // skipped rather than recorded as a multi-second outlier.
    std::int64_t previousEntryNs_ = 0;
    bool havePreviousEntry_ = false;
    std::int64_t intervalCount_ = 0;
    std::int64_t intervalSumUs_ = 0;
    std::int64_t intervalMinUs_ = 0;
    std::int64_t intervalMaxUs_ = 0;
    jamn::core::Histogram64 intervals_;
};

}  // namespace jamn::platform
