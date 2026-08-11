#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <string>

#include <juce_audio_devices/juce_audio_devices.h>

#include "jamn_core/file_audio_device.h"

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

    JuceAudioDevice();
    ~JuceAudioDevice() override;

    JuceAudioDevice(const JuceAudioDevice&) = delete;
    JuceAudioDevice& operator=(const JuceAudioDevice&) = delete;

    // Returns an empty string on success, or a human-readable reason on
    // failure. Never throws and never asserts - a machine with no sound
    // card is an ordinary outcome, not a crash.
    std::string Open(int numOutputChannels, PrepareCallback prepare, jamn::core::AudioCallback callback);
    void Close();

    bool isOpen() const noexcept { return open_; }
    std::string deviceName() const;
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    int blockSize() const noexcept { return blockSize_.load(std::memory_order_relaxed); }

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

    static constexpr int kMaxOutputChannels = 32;

    juce::AudioDeviceManager deviceManager_;
    PrepareCallback prepare_;
    jamn::core::AudioCallback callback_;
    std::array<float*, kMaxOutputChannels> channelPointers_{};
    std::atomic<double> sampleRate_{0.0};
    std::atomic<int> blockSize_{0};
    bool open_ = false;
};

}  // namespace jamn::platform
