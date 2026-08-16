#pragma once

#include <cstdint>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace jamn::ui {

// The on-screen piano: T6.1's mouse half. Click a key to play it, drag
// across the keys to glide, release to stop.
//
// Not called a "strip" on purpose - T6.2's per-peer roster and mixer
// widgets are the strips, and two unrelated things by that name in one
// window is a rename waiting to happen. Not to be confused with
// jamn_platform's KeyNoteInput either: that one is the *computer*
// keyboard's rollover and note-mapping core, and this is a component with
// piano keys drawn on it. They share nothing but a destination.
//
// Follows JamWindowContent's convention exactly: it reports what the user
// did through std::functions on the message thread and knows nothing
// about what happens next. jamn_app wires these to the same
// SubmitLocalNote entry point the computer keyboard uses, so a
// mouse-played note is indistinguishable from a typed one by the time it
// reaches the ring.

// Which pitch a point falls on, or -1 for none. A free function taking no
// JUCE types, because it is the only part of this widget with a decision
// in it - so it is testable with no display, no window and no mouse,
// which is the whole of what a session can verify here.
//
// Black keys are tested before white ones: they are drawn on top and
// overlap the white keys' upper portion, so a point in that region is on
// the black key even though it is also within a white key's column.
int PitchAtPoint(int width, int height, int x, int y, std::uint8_t lowestPitch, int whiteKeyCount);

class NoteKeyboard final : public juce::Component {
public:
    // Two octaves from C3, which is the range the computer keymap's lower
    // row covers - so the two input paths look like the same instrument
    // rather than two unrelated ones.
    static constexpr std::uint8_t kLowestPitch = 48;
    static constexpr int kWhiteKeyCount = 15;

    NoteKeyboard();

    std::function<void(std::uint8_t)> onNotePressed;   // message thread
    std::function<void(std::uint8_t)> onNoteReleased;  // message thread

    // Releases whatever the mouse is holding, through the ordinary
    // callback - so peers hear the note end too. Panic silences this
    // machine's own sound; without this the widget would still believe a
    // key is down, and a peer would be left with a note nothing ever
    // ends.
    void ReleaseHeldNote();

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

private:
    // Releases whatever is sounding and starts `pitch`, or releases only
    // if `pitch` is -1. One place, because every mouse path is some
    // version of "the held note changed".
    void MoveTo(int pitch);

    // -1 when nothing is held. A drag that leaves the component holds
    // nothing rather than holding the last key it touched: a note whose
    // release depends on the mouse coming back is a stuck note.
    int heldPitch_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NoteKeyboard)
};

}  // namespace jamn::ui
