#pragma once

// What every part of the rule editor's controller uses and nothing else does: the string guard
// every shown text goes through. Out of `rules_controller.cpp` since 2026-09-28.

#include "core/io/utf8.hpp"

#include <slint.h>

#include <string>

namespace takt4::ui::rules_detail {

/// Every string this editor shows, made safe to show — see `io::validUtf8`. A target is named
/// after its MIDI device unless somebody named it, and that name is the driver's, in whatever
/// encoding the driver used; one byte Slint cannot decode is an abort.
inline slint::SharedString shared(const std::string& text) {
    return slint::SharedString(io::validUtf8(text));
}

} // namespace takt4::ui::rules_detail
