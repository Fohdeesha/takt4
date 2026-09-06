#pragma once

#include "core/control/control_action.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::control {

/// One MIDI message, reduced to the three things a binding cares about.
///
/// Not every MIDI message — this is what a *control surface* sends when a button is
/// pressed or a knob turned, which is what §5.7's learn mode is for. System messages,
/// pitch bend, aftertouch and program change are none of those, and a binding table that
/// accepted them would be inviting an operator to bind a clock tick.
struct MidiEvent {
    enum class Kind : std::uint8_t {
        Note,          ///< note-on with a non-zero velocity
        ControlChange, ///< CC, whatever its value
    };

    Kind kind = Kind::Note;
    std::uint8_t channel = 1; ///< 1-16, as every piece of hardware prints it
    std::uint8_t number = 0;  ///< note number or CC number, 0-127
    std::uint8_t value = 0;   ///< velocity or CC value, 0-127

    friend bool operator==(const MidiEvent&, const MidiEvent&) = default;
};

/// Reads one raw MIDI message. Nothing for anything that is not a note-on or a CC —
/// including a note-*off*, and including a note-on of velocity 0, which is the note-off
/// most hardware actually sends. Binding a release rather than a press would put a tap on
/// the wrong edge, which for a tap tempo is the whole of the measurement.
std::optional<MidiEvent> readMidiEvent(std::span<const unsigned char> message) noexcept;

/// A control surface's control bound to one action.
///
/// The *value* is deliberately not part of the identity: a binding is "this button", and
/// what it sends when pressed is the argument, not part of which control it is. That is
/// what lets one CC bound to `lock` both pin and release — the fader's own position says
/// which — instead of needing two bindings that could never both be learned from one
/// gesture.
///
/// A `ControlTarget` rather than a bare `ControlAction`, because §5.7's
/// `rule/<id>/enable` names a rule and a binding to it is worthless without one. The
/// implicit conversion means a binding to any of the other six still reads as before.
struct MidiBinding {
    MidiEvent::Kind kind = MidiEvent::Kind::Note;
    std::uint8_t channel = 1;
    std::uint8_t number = 0;
    ControlTarget target;

    /// True when `event` is this control, whatever it is currently sending.
    bool matches(const MidiEvent& event) const noexcept {
        return event.kind == kind && event.channel == channel && event.number == number;
    }

    friend bool operator==(const MidiBinding&, const MidiBinding&) = default;
};

/// "note 36 ch 10 -> tap", the form a settings file stores and a UI shows — and
/// "cc 21 ch 1 -> rule/intro/enable" for the one action that names something. Round-trips
/// through `parseMidiBinding`.
std::string formatMidiBinding(const MidiBinding& binding);

/// The inverse. Nothing when the text is not a binding — an unknown verb, a number out of
/// range, a missing field. Never throws: this reads a file a person may have edited.
std::optional<MidiBinding> parseMidiBinding(std::string_view text) noexcept;

/// What a control that is not a switch should hand an action that wants `<0|1>`.
///
/// A note-on is a press, so it means 1 and a binding to `lock` pins. A CC is a position,
/// so 0-63 is off and 64-127 is on — the convention every hardware switch already uses,
/// and the one that makes a sustain pedal or a toggle button work without configuration.
double argumentOf(const MidiEvent& event) noexcept;

} // namespace takt4::control
