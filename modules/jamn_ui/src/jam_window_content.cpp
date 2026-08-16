#include "jamn_ui/jam_window_content.h"

namespace jamn::ui {

JamWindowContent::JamWindowContent() {
    addAndMakeVisible(blipButton_);
    blipButton_.onClick = [this] {
        if (onButtonClicked) {
            onButtonClicked();
        }
    };

    // Red, and the only coloured control in the window. A player reaching
    // for it is not reading labels.
    panicButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::darkred);
    addAndMakeVisible(panicButton_);
    panicButton_.onClick = [this] { FirePanic(); };

    volumeSlider_.setRange(0.0, 1.0);
    volumeSlider_.setValue(kDefaultGain, juce::dontSendNotification);
    addAndMakeVisible(volumeSlider_);
    volumeSlider_.onValueChange = [this] {
        if (onGainChanged) {
            onGainChanged(static_cast<float>(volumeSlider_.getValue()));
        }
    };

    // Forwarded rather than handled: this class knows no more about a
    // note than it does about a gain.
    addAndMakeVisible(keyboard_);
    keyboard_.onNotePressed = [this](std::uint8_t pitch) {
        if (onNotePressed) {
            onNotePressed(pitch);
        }
    };
    keyboard_.onNoteReleased = [this](std::uint8_t pitch) {
        if (onNoteReleased) {
            onNoteReleased(pitch);
        }
    };

    // So Escape has somewhere to land. JUCE bubbles an unhandled key press
    // from the focused component up through its parents, so this catches
    // it whether focus sits on a child control or on nothing in
    // particular - which is where it ends up after clicking the piano,
    // since NoteKeyboard does not want focus itself.
    setWantsKeyboardFocus(true);

    setSize(620, 300);
}

void JamWindowContent::FirePanic() {
    // Release before panicking, so the note-off reaches peers by the
    // ordinary route. Panic itself is local.
    keyboard_.ReleaseHeldNote();
    if (onPanicClicked) {
        onPanicClicked();
    }
}

bool JamWindowContent::keyPressed(const juce::KeyPress& key) {
    if (key == juce::KeyPress::escapeKey) {
        FirePanic();
        return true;
    }
    return false;
}

void JamWindowContent::resized() {
    auto bounds = getLocalBounds().reduced(20);
    auto buttonRow = bounds.removeFromTop(40);
    // Panic takes the right third and sits apart from the blip button, so
    // it is not somewhere a hand lands by accident.
    panicButton_.setBounds(buttonRow.removeFromRight(buttonRow.getWidth() / 3).reduced(4, 0));
    blipButton_.setBounds(buttonRow);
    bounds.removeFromTop(20);
    volumeSlider_.setBounds(bounds.removeFromTop(40));
    bounds.removeFromTop(20);
    // Whatever is left, so the keys grow with the window rather than
    // sitting at a fixed size in the middle of it.
    keyboard_.setBounds(bounds);
}

}  // namespace jamn::ui
