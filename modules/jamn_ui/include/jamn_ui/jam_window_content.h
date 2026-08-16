#pragma once

#include <cstdint>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "jamn_ui/note_keyboard.h"

namespace jamn::ui {

// The window: one button, one slider, and T6.1's on-screen piano.
//
// Deliberately has no edge to jamn_dsp. It reports what the user did
// through these std::functions and knows nothing about what happens
// next - jamn_app is the only place that wires both halves together. The
// piano's two callbacks are forwarded rather than re-implemented, so
// there is still exactly one place a widget's output is described.
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

    // Stop every sound now. Fired after the piano has released whatever
    // it was holding, so a peer is not left with a note this machine
    // silenced locally and never ended on the wire.
    //
    // Reachable from the button and from Escape. The key binding is not a
    // convenience: with one mouse you cannot hold a note *and* click the
    // button, because letting go to click is itself the note-off - so
    // without it, the one situation panic exists for is the one situation
    // it cannot be reached in.
    std::function<void()> onPanicClicked;  // message thread

    // The piano's, forwarded. Wired in jamn_app to the same entry point
    // the computer keyboard uses.
    std::function<void(std::uint8_t)> onNotePressed;   // message thread
    std::function<void(std::uint8_t)> onNoteReleased;  // message thread

    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    // The one panic path, so the button and the key cannot drift apart.
    void FirePanic();

    juce::TextButton blipButton_{"Blip"};
    juce::TextButton panicButton_{"Panic (Esc)"};
    juce::Slider volumeSlider_{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
    NoteKeyboard keyboard_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JamWindowContent)
};

}  // namespace jamn::ui
