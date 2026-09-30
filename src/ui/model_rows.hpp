#pragma once

#include <slint.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace takt4::ui {

/// Writes `rows` into `model` without resetting it.
///
/// **Why this exists rather than `set_vector`.** `VectorModel::set_vector` calls
/// `notify_reset()`, which tells every repeater bound to the model that nothing it holds
/// is valid any more — so the repeater destroys its items and builds new ones. An item
/// rebuilt that way is a fresh element tree: a text box that had the keyboard loses it
/// mid-word, and a slider being dragged loses the grab on the first pixel of movement.
///
/// That is fine for a list an operator just edited, and ruinous for one a timer refreshes.
/// `RulesController::tick` republishes the rules and their generator slots every time a
/// rule fires — which, for a rule on beats, is every beat of the music — and the trigger
/// editor is full of text boxes. The rows were being replaced under the cursor several
/// times a second, so a rule could not be typed into while anything was playing, which is
/// the only time anybody edits one.
///
/// `notify_row_changed` instead asks the repeater to update the item it already has, so
/// the element — and the focus, and the drag — survives. Rows that did not move are not
/// written at all, which matters because a row carrying a "last produced" readout changes
/// on every beat while the fields beside it are being edited.
///
/// Adding and removing rows goes through `push_back`/`erase` for the same reason: those
/// notify incrementally, and only the rows after the change are disturbed.
///
/// **An updated row is always enough now.** A std widget that assigns its own value — a
/// `ComboBox` picked from, a `CheckBox` clicked — loses its binding to the model the moment it is
/// used, and until 2026-09-30 the rule editor built such a row again as a new element to bring
/// the binding back (the "editing one changes them all" bug, and the colour picker's crash when
/// the rebuild took a popup away under the slider being dragged). Every control in the windows
/// that use this is weltformat.slint's since the Weltformat-dark redesign, and none of those ever
/// assigns its own value, so no row has to be built again.
template <typename Row>
void writeRows(slint::VectorModel<Row>& model, const std::vector<Row>& rows) {
    while (model.row_count() > rows.size()) {
        model.erase(model.row_count() - 1);
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i >= model.row_count()) {
            model.push_back(rows[i]);
        } else if (model.row_data(i) != rows[i]) {
            model.set_row_data(i, rows[i]);
        }
    }
}

/// A repeater's model, written in place (`writeRows`) or emptied — which is how another rule's
/// rows are built from nothing, so a box that had the keyboard does not go on holding what was
/// typed for the rule before.
template <typename Row>
class Repeater {
public:
    Repeater() : model_(std::make_shared<slint::VectorModel<Row>>()) {}

    /// What the window's repeater is bound to.
    [[nodiscard]] const std::shared_ptr<slint::VectorModel<Row>>& model() const noexcept {
        return model_;
    }

    /// Empties it, so the window throws its items away and builds new ones.
    void clear() { model_->clear(); }

    /// `writeRows`: the model updated to `rows` in place.
    void write(const std::vector<Row>& rows) { writeRows(*model_, rows); }

private:
    std::shared_ptr<slint::VectorModel<Row>> model_;
};

} // namespace takt4::ui
