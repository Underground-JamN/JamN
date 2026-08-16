// T6.1: the keyboard core, fed a synthetic stream of (rawCode, pressed)
// pairs - no window, no display, no keyboard, and nothing platform
// -specific. The three capture backends are what cannot be tested here;
// everything they decide is decided in this file's subject instead.
//
// **The first test target jamn_platform has had.** Labelled `app`, not
// `fast`: this module links JUCE for the audio device, and `ctest -L
// fast` must hold the same list under core-only and the full build
// (AGENTS.md, Fast path). The subject itself includes no JUCE at all -
// the label follows the module, not the code under test.
//
// Every TEST_CASE name here contains "key_note_input", not only the tag.
// `ctest -R` matches test names and never tags, and exits 0 on an empty
// selection. The accept command is:
//
//     ctest --preset linux-ninja -R 'key_note_input'   -> 9 tests

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include "jamn_platform/key_note_input.h"

using jamn::platform::BuildBindings;
using jamn::platform::KeyBinding;
using jamn::platform::KeyNoteInput;
using jamn::platform::kKeyPositions;
using jamn::platform::kLayoutPitches;

namespace {

// Stand-ins for a backend's raw codes. Their values mean nothing beyond
// being distinct - which is the property the core actually relies on, and
// the reason it can be tested without any real OS's numbering.
std::array<std::uint32_t, kKeyPositions> SyntheticRawCodes() {
    std::array<std::uint32_t, kKeyPositions> codes{};
    for (std::size_t index = 0; index < kKeyPositions; ++index) {
        codes[index] = static_cast<std::uint32_t>(100 + index);
    }
    return codes;
}

KeyNoteInput MakeInput(const std::array<std::uint32_t, kKeyPositions>& codes) {
    std::array<KeyBinding, kKeyPositions> bindings{};
    const std::size_t count = BuildBindings(codes.data(), codes.size(), bindings.data(), bindings.size());
    REQUIRE(count == kKeyPositions);

    KeyNoteInput input;
    input.SetBindings(bindings.data(), count);
    return input;
}

}  // namespace

TEST_CASE("key_note_input plays a note on press and ends it on release", "[key_note_input]") {
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    REQUIRE(input.OnKey(codes[0], true, event));
    REQUIRE(event.kind == KeyNoteInput::Event::Kind::kNoteOn);
    REQUIRE(event.pitch == kLayoutPitches[0]);
    REQUIRE(input.HeldCount() == 1);

    REQUIRE(input.OnKey(codes[0], false, event));
    REQUIRE(event.kind == KeyNoteInput::Event::Kind::kNoteOff);
    REQUIRE(event.pitch == kLayoutPitches[0]);
    REQUIRE(input.HeldCount() == 0);
}

TEST_CASE("key_note_input ignores a key it has no binding for", "[key_note_input]") {
    // Most of the keyboard. A backend hands over every key the window
    // sees, so this is the common case rather than an error one.
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    REQUIRE_FALSE(input.OnKey(9999, true, event));
    REQUIRE_FALSE(input.OnKey(9999, false, event));
    REQUIRE(input.HeldCount() == 0);
}

TEST_CASE("key_note_input filters an auto-repeated press", "[key_note_input]") {
    // X11 has no repeat bit at all - a repeat is an ordinary press - so
    // the held set is what has to filter it. Without this, holding a key
    // machine-guns note-ons at the OS's repeat rate.
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    REQUIRE(input.OnKey(codes[3], true, event));
    for (int repeat = 0; repeat < 20; ++repeat) {
        REQUIRE_FALSE(input.OnKey(codes[3], true, event));
    }
    REQUIRE(input.HeldCount() == 1);

    REQUIRE(input.OnKey(codes[3], false, event));
    REQUIRE(event.kind == KeyNoteInput::Event::Kind::kNoteOff);
}

TEST_CASE("key_note_input holds six keys and ignores a seventh", "[key_note_input]") {
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    for (std::size_t index = 0; index < KeyNoteInput::kMaxHeldKeys; ++index) {
        REQUIRE(input.OnKey(codes[index], true, event));
    }
    REQUIRE(input.HeldCount() == KeyNoteInput::kMaxHeldKeys);

    REQUIRE_FALSE(input.OnKey(codes[KeyNoteInput::kMaxHeldKeys], true, event));
    REQUIRE(input.KeysIgnoredOverRollover() == 1);
    REQUIRE(input.HeldCount() == KeyNoteInput::kMaxHeldKeys);

    // And its release is silent too: nothing sounded, so nothing is
    // silenced. Emitting a note-off here would cut whichever of the six
    // happened to share its pitch.
    REQUIRE_FALSE(input.OnKey(codes[KeyNoteInput::kMaxHeldKeys], false, event));
}

TEST_CASE("key_note_input frees a rollover slot when a key is released", "[key_note_input]") {
    // The cap is on keys down at once, not on keys ever pressed - a
    // player who never lifts six fingers must not stop being able to
    // play after six notes.
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    for (std::size_t index = 0; index < KeyNoteInput::kMaxHeldKeys; ++index) {
        REQUIRE(input.OnKey(codes[index], true, event));
    }
    REQUIRE(input.OnKey(codes[0], false, event));
    REQUIRE(input.OnKey(codes[KeyNoteInput::kMaxHeldKeys], true, event));
    REQUIRE(event.kind == KeyNoteInput::Event::Kind::kNoteOn);
    REQUIRE(input.HeldCount() == KeyNoteInput::kMaxHeldKeys);
}

TEST_CASE("key_note_input keeps a pitch sounding while its other key is held", "[key_note_input]") {
    // The layout's two rows overlap by a fifth, so `,` and `Q` are both
    // C4. NoteOff silences every voice at a pitch, so releasing one of
    // them while the other is down would cut a note the player is still
    // holding. Position 12 and position 17 are that pair.
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);
    REQUIRE(kLayoutPitches[12] == kLayoutPitches[17]);

    KeyNoteInput::Event event;
    REQUIRE(input.OnKey(codes[12], true, event));
    REQUIRE(event.pitch == kLayoutPitches[12]);
    // A retrigger, deliberately - a player pressing a key expects to hear
    // it, and the instrument's contract covers two note-ons for a pitch.
    REQUIRE(input.OnKey(codes[17], true, event));
    REQUIRE(event.kind == KeyNoteInput::Event::Kind::kNoteOn);

    REQUIRE_FALSE(input.OnKey(codes[12], false, event));
    REQUIRE(input.HeldCount() == 1);

    // The last key holding it does end it.
    REQUIRE(input.OnKey(codes[17], false, event));
    REQUIRE(event.kind == KeyNoteInput::Event::Kind::kNoteOff);
    REQUIRE(event.pitch == kLayoutPitches[17]);
}

TEST_CASE("key_note_input releases everything held when focus is lost", "[key_note_input]") {
    // Capture is window-focused, so a key held while alt-tabbing away is
    // a key whose release this process will never see. A stuck note is
    // worse than a dropped one.
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    REQUIRE(input.OnKey(codes[0], true, event));
    REQUIRE(input.OnKey(codes[2], true, event));
    REQUIRE(input.OnKey(codes[4], true, event));

    std::array<KeyNoteInput::Event, KeyNoteInput::kMaxHeldKeys> released{};
    const std::size_t count = input.ReleaseAll(released.data(), released.size());

    REQUIRE(count == 3);
    for (std::size_t index = 0; index < count; ++index) {
        REQUIRE(released[index].kind == KeyNoteInput::Event::Kind::kNoteOff);
    }
    REQUIRE(input.HeldCount() == 0);

    // And the keys really are forgotten: a release arriving after focus
    // came back must not silence a note played since.
    REQUIRE_FALSE(input.OnKey(codes[0], false, event));
}

TEST_CASE("key_note_input emits one note-off per pitch when focus is lost", "[key_note_input]") {
    const auto codes = SyntheticRawCodes();
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    REQUIRE(input.OnKey(codes[12], true, event));
    REQUIRE(input.OnKey(codes[17], true, event));

    std::array<KeyNoteInput::Event, KeyNoteInput::kMaxHeldKeys> released{};
    const std::size_t count = input.ReleaseAll(released.data(), released.size());

    // Two keys, one pitch, one note-off - the second would be for a pitch
    // already silent.
    REQUIRE(count == 1);
    REQUIRE(released[0].pitch == kLayoutPitches[12]);
    REQUIRE(input.HeldCount() == 0);
}

TEST_CASE("key_note_input maps physical position to pitch, not raw code order", "[key_note_input]") {
    // A backend's raw codes are whatever its OS numbers them; the layout
    // is what decides pitch. Shuffling the codes must move which key
    // plays what, and must not move the layout itself.
    std::array<std::uint32_t, kKeyPositions> codes{};
    for (std::size_t index = 0; index < kKeyPositions; ++index) {
        codes[index] = static_cast<std::uint32_t>(9000 - index * 7);
    }
    KeyNoteInput input = MakeInput(codes);

    KeyNoteInput::Event event;
    REQUIRE(input.OnKey(codes[5], true, event));
    REQUIRE(event.pitch == kLayoutPitches[5]);

    // The lower row starts at C3 and the upper at C4 - the whole of the
    // layout decision, asserted rather than left in a comment.
    REQUIRE(kLayoutPitches[0] == 48);
    REQUIRE(kLayoutPitches[17] == 60);
    REQUIRE(kLayoutPitches[kKeyPositions - 1] == 76);
}
