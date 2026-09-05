#include "core/control/midi_binding.hpp"

#include <charconv>
#include <cstddef>
#include <string>

namespace takt4::control {
namespace {

constexpr unsigned char kStatusMask = 0xF0;
constexpr unsigned char kChannelMask = 0x0F;
constexpr unsigned char kNoteOn = 0x90;
constexpr unsigned char kControlChange = 0xB0;

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

/// A 0-127 field. Nothing when it is not a number or is out of range — a file a person
/// edited is exactly where "note 300" comes from.
std::optional<std::uint8_t> readByte(std::string_view text) noexcept {
    text = trim(text);
    unsigned int value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value > 127) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

} // namespace

std::optional<MidiEvent> readMidiEvent(std::span<const unsigned char> message) noexcept {
    if (message.size() < 3) {
        return std::nullopt;
    }
    const unsigned char status = static_cast<unsigned char>(message[0] & kStatusMask);
    MidiEvent event;
    event.channel = static_cast<std::uint8_t>((message[0] & kChannelMask) + 1);
    event.number = static_cast<std::uint8_t>(message[1] & 0x7F);
    event.value = static_cast<std::uint8_t>(message[2] & 0x7F);

    if (status == kNoteOn) {
        // Velocity 0 is the note-off most hardware sends rather than 0x80. Binding the
        // release instead of the press would put a tap on the wrong edge, and for a tap
        // tempo the edge is the entire measurement.
        if (event.value == 0) {
            return std::nullopt;
        }
        event.kind = MidiEvent::Kind::Note;
        return event;
    }
    if (status == kControlChange) {
        event.kind = MidiEvent::Kind::ControlChange;
        return event;
    }
    return std::nullopt;
}

std::string formatMidiBinding(const MidiBinding& binding) {
    std::string text(binding.kind == MidiEvent::Kind::Note ? "note " : "cc ");
    text += std::to_string(static_cast<unsigned int>(binding.number));
    text += " ch ";
    text += std::to_string(static_cast<unsigned int>(binding.channel));
    text += " -> ";
    text += verbOf(binding.action);
    return text;
}

std::optional<MidiBinding> parseMidiBinding(std::string_view text) noexcept {
    const std::size_t arrow = text.find("->");
    if (arrow == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<ControlAction> action = actionOf(trim(text.substr(arrow + 2)));
    if (!action) {
        return std::nullopt;
    }

    std::string_view control = trim(text.substr(0, arrow));
    MidiBinding binding;
    binding.action = *action;

    if (control.starts_with("note ")) {
        binding.kind = MidiEvent::Kind::Note;
        control.remove_prefix(5);
    } else if (control.starts_with("cc ")) {
        binding.kind = MidiEvent::Kind::ControlChange;
        control.remove_prefix(3);
    } else {
        return std::nullopt;
    }

    const std::size_t ch = control.find(" ch ");
    if (ch == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<std::uint8_t> number = readByte(control.substr(0, ch));
    const std::optional<std::uint8_t> channel = readByte(control.substr(ch + 4));
    // Channels are 1-16 as the hardware prints them, so 0 is not one of them.
    if (!number || !channel || *channel < 1 || *channel > 16) {
        return std::nullopt;
    }
    binding.number = *number;
    binding.channel = *channel;
    return binding;
}

double argumentOf(const MidiEvent& event) noexcept {
    if (event.kind == MidiEvent::Kind::Note) {
        return 1.0; // a press
    }
    // The convention every hardware switch already uses, so a sustain pedal or a toggle
    // button works with no configuration at all.
    return event.value >= 64 ? 1.0 : 0.0;
}

} // namespace takt4::control
