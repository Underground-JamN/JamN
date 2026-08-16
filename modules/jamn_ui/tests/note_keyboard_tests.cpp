// T6.1: the on-screen piano's hit testing, which is the only part of that
// widget with a decision in it. No Component is constructed here and no
// display is touched - PitchAtPoint takes ints and returns an int
// precisely so this file can exist in a session that has neither.
//
// **The first test target jamn_ui has had.** Labelled `app`, not `fast`:
// this module links juce_gui_basics, and `ctest -L fast` must hold the
// same list under core-only and the full build (AGENTS.md, Fast path).
//
// What this cannot cover: that a mouse press reaches mouseDown at all,
// that the keys are drawn where they are tested for, or that any of it
// looks right. Those are the maintainer's, at a real desktop.
//
// Every TEST_CASE name here contains "note_keyboard", not only the tag.
// `ctest -R` matches test names and never tags, and exits 0 on an empty
// selection. The accept command is:
//
//     ctest --preset linux-ninja -R 'note_keyboard'   -> 6 tests

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "jamn_ui/note_keyboard.h"

using jamn::ui::NoteKeyboard;
using jamn::ui::PitchAtPoint;

namespace {

constexpr int kWidth = 600;
constexpr int kHeight = 120;
constexpr std::uint8_t kLowest = NoteKeyboard::kLowestPitch;
constexpr int kWhites = NoteKeyboard::kWhiteKeyCount;

// Well below the black keys, so a hit there is unambiguously the white
// key underneath.
constexpr int kLowY = kHeight - 5;

int At(int x, int y) { return PitchAtPoint(kWidth, kHeight, x, y, kLowest, kWhites); }

}  // namespace

TEST_CASE("note_keyboard maps the left edge to its lowest key", "[note_keyboard]") {
    REQUIRE(At(0, kLowY) == kLowest);
}

TEST_CASE("note_keyboard maps the white keys in order across its width", "[note_keyboard]") {
    // Every white key in turn, sampled at its own centre. Asserting the
    // sequence rather than one key is what catches an off-by-one in the
    // octave arithmetic, which repeats every seven keys and so survives a
    // single-key check.
    const double whiteWidth = static_cast<double>(kWidth) / kWhites;
    constexpr int kExpected[kWhites] = {48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67, 69, 71, 72};

    for (int index = 0; index < kWhites; ++index) {
        const int x = static_cast<int>((index + 0.5) * whiteWidth);
        REQUIRE(At(x, kLowY) == kExpected[index]);
    }
}

TEST_CASE("note_keyboard puts a black key over the boundary it straddles", "[note_keyboard]") {
    // The overlap is the whole point: the same x is a black key near the
    // top and the white key beneath it lower down.
    const double whiteWidth = static_cast<double>(kWidth) / kWhites;
    const int boundary = static_cast<int>(whiteWidth);  // between C and D

    REQUIRE(At(boundary, 5) == kLowest + 1);       // C#
    REQUIRE(At(boundary, kLowY) == kLowest + 2);   // D, below the black key
}

TEST_CASE("note_keyboard has no black key between E and F or B and C", "[note_keyboard]") {
    // The two places a piano's keys sit flush. Getting this wrong is the
    // classic way a generated keyboard ends up with twelve identical
    // white keys and a black one everywhere.
    const double whiteWidth = static_cast<double>(kWidth) / kWhites;

    const int eToF = static_cast<int>(3 * whiteWidth);  // E is white index 2
    REQUIRE(At(eToF, 5) == 53);                          // F, not E#

    const int bToC = static_cast<int>(7 * whiteWidth);  // B is white index 6
    REQUIRE(At(bToC, 5) == 60);                          // C, not B#
}

TEST_CASE("note_keyboard reports nothing outside its bounds", "[note_keyboard]") {
    // What a drag leaving the component produces, and the reason a note
    // does not stick when the mouse wanders off it.
    REQUIRE(At(-1, kLowY) == -1);
    REQUIRE(At(kWidth, kLowY) == -1);
    REQUIRE(At(10, -1) == -1);
    REQUIRE(At(10, kHeight) == -1);
    REQUIRE(PitchAtPoint(0, kHeight, 0, 0, kLowest, kWhites) == -1);
    REQUIRE(PitchAtPoint(kWidth, kHeight, 10, 10, kLowest, 0) == -1);
}

TEST_CASE("note_keyboard keeps its rightmost pixel on the last key", "[note_keyboard]") {
    // Integer division at the right edge is where a keyboard grows a
    // sixteenth white key nobody drew.
    REQUIRE(At(kWidth - 1, kLowY) == 72);
}
