#include "jamn_ui/jam_window_content.h"

namespace jamn::ui {

JamWindowContent::JamWindowContent() {
    addAndMakeVisible(blipButton_);
    blipButton_.onClick = [this] {
        if (onButtonClicked) {
            onButtonClicked();
        }
    };

    volumeSlider_.setRange(0.0, 1.0);
    volumeSlider_.setValue(kDefaultGain, juce::dontSendNotification);
    addAndMakeVisible(volumeSlider_);
    volumeSlider_.onValueChange = [this] {
        if (onGainChanged) {
            onGainChanged(static_cast<float>(volumeSlider_.getValue()));
        }
    };

    setSize(380, 150);
}

void JamWindowContent::resized() {
    auto bounds = getLocalBounds().reduced(20);
    blipButton_.setBounds(bounds.removeFromTop(40));
    bounds.removeFromTop(20);
    volumeSlider_.setBounds(bounds.removeFromTop(40));
}

}  // namespace jamn::ui
