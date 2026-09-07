#pragma once

#include <slint.h>

#include <cstddef>
#include <vector>

namespace takt4::ui {

/// Writes `rows` into `model` without resetting it.
///
/// **Why this exists rather than `set_vector`.** `VectorModel::set_vector` calls
/// `notify_reset()`, which tells every repeater bound to the model that nothing it holds
/// is valid any more — so the repeater destroys its items and builds new ones. An item
/// rebuilt that way is a fresh element tree: a `LineEdit` that had the keyboard loses it
/// mid-word, and a `Slider` being dragged loses the grab on the first pixel of movement.
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
/// One consequence worth stating. A `LineEdit` whose `text:` is bound to a model field
/// loses that binding the moment somebody types into it — Slint drops a binding when the
/// property is assigned — so a value the controller normalises after an edit no longer
/// reappears in the box on its own. Before this it did, because the box was a new one.
/// Keeping what was typed is the better of the two behaviours, and the normalised value
/// shows the next time the row is built.
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

} // namespace takt4::ui
