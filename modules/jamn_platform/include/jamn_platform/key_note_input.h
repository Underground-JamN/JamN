#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace jamn::platform {

// Turns a stream of raw key transitions into note-ons and note-offs:
// rollover, auto-repeat filtering, and the map from a physical key
// position to a pitch. T6.1's keyboard half, minus the capture.
//
// **No JUCE and no OS types anywhere in here**, even though this module
// links JUCE for the audio device - the three per-OS capture backends
// hand it a raw code and a bool, and everything with a decision in it
// lives on this side of that line. So it is testable by feeding it a
// synthetic stream, with no window, no display and no keyboard.
//
// Message thread only. Nothing here is called from the audio thread: the
// events it returns go to jamn_app's one local-input entry point, which
// is where the crossing to the audio thread happens.

// The layout, defined once. A physical *position* is an index into this
// table, and that index is the entire contract between this file and a
// backend's raw-code table - entry N there and entry N here are the same
// key. The layout can be changed here without touching three OS tables,
// which is the point of splitting it this way.
//
// This is the two-row layout most DAWs' computer-keyboard note entry
// uses, keyed on physical position rather than on the character the OS
// says a key produces - so it is unchanged on AZERTY, Dvorak, or any
// other configured text layout, which is why juce::KeyPress was ruled out
// upstream of this file.
//
//   lower row, C3 up:  Z S X D C V G B H N J M , L . ; /
//   upper row, C4 up:  Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P
//
// Each row is ten white keys with the black keys interleaved on the row
// above it. **The rows overlap by a fifth** - the lower row runs C3 to
// E4 and the upper C4 to E5 - which is inherent to the layout, not an
// off-by-one: `,` and `Q` are both C4. See KeyNoteInput's note-off rule
// for what that costs.
inline constexpr std::size_t kKeyPositions = 34;

inline constexpr std::array<std::uint8_t, kKeyPositions> kLayoutPitches = {
    // Lower row: Z S X D C V G B H N J M , L . ; /
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    // Upper row: Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76,
};

// One physical key and the pitch it plays. Backends build these by
// zipping their own raw codes against kLayoutPitches - see
// BuildBindings.
struct KeyBinding {
    std::uint32_t rawCode = 0;
    std::uint8_t pitch = 0;
};

// Zips a backend's raw codes, given in kLayoutPitches' order, into
// bindings. Returns how many were written, which is the smaller of
// `count`, `capacity` and kKeyPositions - a backend that maps only part
// of the layout (a keyboard with no numeric row, say) gets the part it
// asked for rather than a silent overrun.
std::size_t BuildBindings(const std::uint32_t* rawCodes, std::size_t count, KeyBinding* out, std::size_t capacity);

class KeyNoteInput {
public:
    // Six simultaneous physical keys, per T6.1's scope. A seventh is
    // ignored outright - not queued, and the set is not grown into.
    // Fixed capacity even off the audio thread, matching this codebase's
    // idiom.
    static constexpr std::size_t kMaxHeldKeys = 6;
    static constexpr std::size_t kMaxBindings = kKeyPositions;

    struct Event {
        enum class Kind : std::uint8_t { kNoteOn, kNoteOff };
        Kind kind = Kind::kNoteOn;
        std::uint8_t pitch = 0;
    };

    // Copies the bindings in; the caller's array need not outlive this.
    // Anything past kMaxBindings is refused rather than truncated
    // silently, and setting bindings does not release held keys - see
    // ReleaseAll for that.
    void SetBindings(const KeyBinding* bindings, std::size_t count);

    // One raw key transition, as a backend saw it. Returns true when
    // `out` was filled, and false for every transition that is correctly
    // silent:
    //
    //   - a raw code no binding names (most of the keyboard),
    //   - a press of a key already held, which is what OS auto-repeat
    //     looks like on X11, where it arrives as an ordinary press with
    //     no bit distinguishing it,
    //   - a press while kMaxHeldKeys are already down,
    //   - a release of a key that was not held, which is the release half
    //     of both cases above.
    bool OnKey(std::uint32_t rawCode, bool pressed, Event& out);

    // Every held key released at once, as note-offs, and the held set
    // cleared. What a backend calls when the window loses focus: capture
    // is window-focused, so a key held while alt-tabbing away is a key
    // whose release this process will never see, and a stuck note is
    // worse than a dropped one (docs/CLOCK.md). Returns how many events
    // were written; `capacity` should be at least kMaxHeldKeys.
    std::size_t ReleaseAll(Event* out, std::size_t capacity);

    std::size_t HeldCount() const { return heldCount_; }

    // Presses refused for being over the rollover cap. Diagnostics only.
    std::uint64_t KeysIgnoredOverRollover() const { return ignoredOverRollover_; }

private:
    // Whether any held key other than `except` plays `pitch`. The layout
    // above binds C4 to two keys, so a note-off cannot be emitted just
    // because *a* key sounding that pitch went up.
    bool AnotherKeyHolds(std::uint8_t pitch, std::size_t except) const;

    struct HeldKey {
        std::uint32_t rawCode = 0;
        std::uint8_t pitch = 0;
    };

    std::array<KeyBinding, kMaxBindings> bindings_{};
    std::size_t bindingCount_ = 0;
    std::array<HeldKey, kMaxHeldKeys> held_{};
    std::size_t heldCount_ = 0;
    std::uint64_t ignoredOverRollover_ = 0;
};

}  // namespace jamn::platform
