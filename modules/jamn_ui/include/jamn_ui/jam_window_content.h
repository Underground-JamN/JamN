#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace jamn::ui {

// Phase 0's whole window: one button, one slider.
//
// Deliberately has no edge to jamn_dsp. It reports what the user did
// through these two std::functions and knows nothing about what happens
// next - jamn_app is the only place that wires both halves together.
class JamWindowContent final : public juce::Component {
public:
    // Also the slider's initial value. Exposed so the caller can bring
    // whatever it wires onGainChanged to (e.g. MasterBus) up to date with
    // what the slider displays at startup - the slider is constructed with
    // dontSendNotification, so onGainChanged never fires on its own here.
    static constexpr float kDefaultGain = 0.8f;

    JamWindowContent();

    std::function<void()> onButtonClicked;    // called on the message thread
    std::function<void(float)> onGainChanged;  // called on the message thread, linear 0..1

    void resized() override;

private:
    juce::TextButton blipButton_{"Blip"};
    juce::Slider volumeSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JamWindowContent)
};

}  // namespace jamn::ui
