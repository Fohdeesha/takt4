#pragma once

// What every part of the rule editor's controller uses and nothing else does: the guard
// for a picker's own publishing and the string guard every shown text goes through. Out of
// `rules_controller.cpp` since 2026-09-28.

#include "core/io/utf8.hpp"

#include <slint.h>

#include <string>

namespace takt4::ui::rules_detail {

/// Holds `RulesController::pickingColor_` up for one call, so that everything a picker's own
/// slider publishes is marked as coming from that slider. Scoped rather than a pair of
/// assignments because the publishers it guards can raise a status, throw, or return early.
class PickingColor {
public:
    explicit PickingColor(bool& flag) noexcept : flag_(flag), was_(flag) { flag_ = true; }
    ~PickingColor() { flag_ = was_; }

    PickingColor(const PickingColor&) = delete;
    PickingColor& operator=(const PickingColor&) = delete;

private:
    bool& flag_;
    bool was_;
};

/// Every string this editor shows, made safe to show — see `io::validUtf8`. A target is named
/// after its MIDI device unless somebody named it, and that name is the driver's, in whatever
/// encoding the driver used; one byte Slint cannot decode is an abort.
inline slint::SharedString shared(const std::string& text) {
    return slint::SharedString(io::validUtf8(text));
}

} // namespace takt4::ui::rules_detail
