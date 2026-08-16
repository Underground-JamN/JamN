#include "jamn_ui/note_keyboard.h"

#include <array>

namespace jamn::ui {
namespace {

// Semitone offsets of the white notes within an octave: C D E F G A B.
constexpr std::array<int, 7> kWhiteOffsets = {0, 2, 4, 5, 7, 9, 11};

// Whether a black key sits immediately right of the white note at this
// index within the octave - true for C, D, F, G, A, false for E and B,
// which is the whole of why a piano's keys are not evenly spaced.
constexpr std::array<bool, 7> kHasBlackToTheRight = {true, true, false, true, true, true, false};

// Proportions of a white key. Roughly a real keyboard's, and the only
// numbers here that are taste rather than arithmetic.
constexpr double kBlackWidthRatio = 0.6;
constexpr double kBlackHeightRatio = 0.6;

// The pitch of the nth white key up from `lowestPitch`, which is assumed
// to be a C.
int WhitePitch(std::uint8_t lowestPitch, int whiteIndex) {
    const int octave = whiteIndex / 7;
    const int within = whiteIndex % 7;
    return lowestPitch + octave * 12 + kWhiteOffsets[static_cast<std::size_t>(within)];
}

}  // namespace

int PitchAtPoint(int width, int height, int x, int y, std::uint8_t lowestPitch, int whiteKeyCount) {
    if (width <= 0 || height <= 0 || whiteKeyCount <= 0) return -1;
    if (x < 0 || y < 0 || x >= width || y >= height) return -1;

    const double whiteWidth = static_cast<double>(width) / whiteKeyCount;
    const double blackWidth = whiteWidth * kBlackWidthRatio;
    const double blackHeight = height * kBlackHeightRatio;

    // Black keys first: they are drawn over the white keys' upper part,
    // so in the overlap the black key is what the player sees and what
    // they meant to hit.
    if (y < blackHeight) {
        for (int whiteIndex = 0; whiteIndex + 1 < whiteKeyCount; ++whiteIndex) {
            if (!kHasBlackToTheRight[static_cast<std::size_t>(whiteIndex % 7)]) continue;

            // Centred on the boundary between this white key and the next.
            const double centre = (whiteIndex + 1) * whiteWidth;
            if (x >= centre - blackWidth / 2.0 && x < centre + blackWidth / 2.0) {
                return WhitePitch(lowestPitch, whiteIndex) + 1;
            }
        }
    }

    int whiteIndex = static_cast<int>(x / whiteWidth);
    // Guards the right edge only: x/whiteWidth can round to whiteKeyCount
    // for a point one pixel inside the last key.
    if (whiteIndex >= whiteKeyCount) whiteIndex = whiteKeyCount - 1;
    return WhitePitch(lowestPitch, whiteIndex);
}

NoteKeyboard::NoteKeyboard() {
    setSize(600, 120);
}

void NoteKeyboard::paint(juce::Graphics& graphics) {
    const auto bounds = getLocalBounds();
    const double whiteWidth = static_cast<double>(bounds.getWidth()) / kWhiteKeyCount;
    const double blackWidth = whiteWidth * kBlackWidthRatio;
    const double blackHeight = bounds.getHeight() * kBlackHeightRatio;

    for (int whiteIndex = 0; whiteIndex < kWhiteKeyCount; ++whiteIndex) {
        const int pitch = WhitePitch(kLowestPitch, whiteIndex);
        const juce::Rectangle<float> key(static_cast<float>(whiteIndex * whiteWidth), 0.0f,
                                          static_cast<float>(whiteWidth), static_cast<float>(bounds.getHeight()));
        graphics.setColour(pitch == heldPitch_ ? juce::Colours::lightblue : juce::Colours::white);
        graphics.fillRect(key);
        graphics.setColour(juce::Colours::black);
        graphics.drawRect(key, 1.0f);
    }

    // Drawn after every white key, not interleaved, so a black key is
    // never painted over by its right-hand neighbour.
    for (int whiteIndex = 0; whiteIndex + 1 < kWhiteKeyCount; ++whiteIndex) {
        if (!kHasBlackToTheRight[static_cast<std::size_t>(whiteIndex % 7)]) continue;

        const int pitch = WhitePitch(kLowestPitch, whiteIndex) + 1;
        const double centre = (whiteIndex + 1) * whiteWidth;
        const juce::Rectangle<float> key(static_cast<float>(centre - blackWidth / 2.0), 0.0f,
                                          static_cast<float>(blackWidth), static_cast<float>(blackHeight));
        graphics.setColour(pitch == heldPitch_ ? juce::Colours::steelblue : juce::Colours::black);
        graphics.fillRect(key);
    }
}

void NoteKeyboard::MoveTo(int pitch) {
    if (pitch == heldPitch_) return;

    if (heldPitch_ >= 0 && onNoteReleased) {
        onNoteReleased(static_cast<std::uint8_t>(heldPitch_));
    }
    heldPitch_ = pitch;
    if (heldPitch_ >= 0 && onNotePressed) {
        onNotePressed(static_cast<std::uint8_t>(heldPitch_));
    }
    repaint();
}

void NoteKeyboard::ReleaseHeldNote() {
    MoveTo(-1);
}

void NoteKeyboard::mouseDown(const juce::MouseEvent& event) {
    MoveTo(PitchAtPoint(getWidth(), getHeight(), event.x, event.y, kLowestPitch, kWhiteKeyCount));
}

void NoteKeyboard::mouseDrag(const juce::MouseEvent& event) {
    // A drag off the component releases rather than holding the last key
    // touched - PitchAtPoint returns -1 out of bounds, and MoveTo treats
    // that as "nothing held".
    MoveTo(PitchAtPoint(getWidth(), getHeight(), event.x, event.y, kLowestPitch, kWhiteKeyCount));
}

void NoteKeyboard::mouseUp(const juce::MouseEvent&) {
    MoveTo(-1);
}

}  // namespace jamn::ui
