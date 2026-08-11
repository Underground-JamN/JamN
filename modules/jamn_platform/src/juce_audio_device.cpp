#include "jamn_platform/juce_audio_device.h"

#include <algorithm>

#include "jamn_core/realtime_scope.h"

namespace jamn::platform {

JuceAudioDevice::JuceAudioDevice() = default;

JuceAudioDevice::~JuceAudioDevice() {
    Close();
}

std::string JuceAudioDevice::Open(int numOutputChannels,
                                   PrepareCallback prepare,
                                   jamn::core::AudioCallback callback) {
    // std::function assignment allocates - confined to Open, never the RT
    // callback, which only ever invokes callback_, and invoking one doesn't
    // allocate.
    prepare_ = std::move(prepare);
    callback_ = std::move(callback);

    const juce::String error = deviceManager_.initialise(0, numOutputChannels, nullptr, true);
    if (error.isNotEmpty()) {
        return error.toStdString();
    }

    // initialise() can return no error and still leave no device selected -
    // this machine has no sound card at all. Trusting the empty error
    // string alone would hand a null device to audioDeviceAboutToStart.
    if (deviceManager_.getCurrentAudioDevice() == nullptr) {
        return "no audio output device is available on this system";
    }

    deviceManager_.addAudioCallback(this);
    open_ = true;
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
    juce::ignoreUnused(inputChannelData, numInputChannels, context);

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

    jamn::core::RealtimeScope scope;
    callback_(channelPointers_.data(), usableChannels, numSamples);
}

void JuceAudioDevice::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    const double rate = device->getCurrentSampleRate();
    const int block = device->getCurrentBufferSizeSamples();
    sampleRate_.store(rate, std::memory_order_relaxed);
    blockSize_.store(block, std::memory_order_relaxed);
    if (prepare_) {
        prepare_(rate, block);
    }
}

void JuceAudioDevice::audioDeviceStopped() {
    sampleRate_.store(0.0, std::memory_order_relaxed);
    blockSize_.store(0, std::memory_order_relaxed);
}

void JuceAudioDevice::audioDeviceError(const juce::String& errorMessage) {
    juce::ignoreUnused(errorMessage);
}

}  // namespace jamn::platform
