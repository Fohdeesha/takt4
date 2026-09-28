#pragma once

#include <slint.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
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
/// One consequence worth stating, because it is the whole of `staleRows` below. A std widget
/// that assigns its own value — a `ComboBox` picked from, a `CheckBox` clicked, a `Slider`
/// dragged — loses its binding to the model field the moment it is used: Slint drops a binding
/// when the property is assigned. So a value the controller changes afterwards no longer
/// reappears in it on its own, and neither does the *next* rule's. That is what a rig met, when
/// the text boxes were `LineEdit`s bound this way, as "when I edit the value range on trigger
/// 1, it changes all the triggers". The rows' text and number boxes are `LiveField` and
/// `NumberBox` now, which never assign their own value and so never need this (the audit of
/// 2026-09-25, M17).
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

/// The surviving rows of `model` that `rows` would move in a way that matters, by index.
///
/// Updating in place keeps the element, which is what stops a box being destroyed under the
/// cursor thirty times a second — and it keeps a *dead binding* with it, so a dropdown that
/// has been picked from goes on showing its pick however often the row beneath it changes.
/// Within one edit that is right. Across a change the controller made itself — a row that
/// moved to another kind, another rule's values — it is the difference between a widget that
/// reports the rule and one that reports the last thing anybody picked anywhere.
///
/// So: a row **added** past the end is a fresh element and always fine; a row **removed** off
/// the end disturbs nothing before it; a *surviving* row whose content moved is the case that
/// needs a rebuild. `changed(was, now)` says whether it moved in a way that matters — callers
/// exclude the fields that update themselves (a readout that ticks over on every beat, whatever
/// a `LiveField` or a `NumberBox` shows), so none of those tears down the row it is on.
template <typename Row, typename Changed>
std::vector<std::size_t> staleRows(const slint::VectorModel<Row>& model,
                                   const std::vector<Row>& rows, Changed changed) {
    std::vector<std::size_t> stale;
    const std::size_t common = std::min<std::size_t>(model.row_count(), rows.size());
    for (std::size_t i = 0; i < common; ++i) {
        const std::optional<Row> was = model.row_data(i);
        if (was && changed(*was, rows[i])) {
            stale.push_back(i);
        }
    }
    return stale;
}

/// Builds the rows at `indices` again as new elements, and nothing else, then forgets them.
///
/// **One row at a time, not the whole list** (the audit's M16). A box typed into loses its
/// binding and has to come back as a new element — but emptying the model to do that threw
/// away every row, and with them the box the operator had *just clicked into*: leaving one
/// chip's box commits it, the commit asks for a rebuild, and the rebuild took the next box out
/// from under the pointer, so what was typed there went nowhere. An erase and an insert at
/// one index make the repeater replace that item alone; every other row keeps its element and
/// whatever has the keyboard keeps it.
template <typename Row>
void renewRows(slint::VectorModel<Row>& model, std::vector<std::size_t>& indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    for (const std::size_t i : indices) {
        if (i >= model.row_count()) {
            continue;
        }
        const std::optional<Row> row = model.row_data(i);
        if (!row) {
            continue;
        }
        model.erase(i);
        model.insert(i, *row);
    }
    indices.clear();
}

/// A repeater's model, and the rows of it that are to be built again as new elements the next
/// time `renew` is called (see `renewRows`).
///
/// **One thing, so they are emptied together.** They were two members each in
/// `RulesController`, and a model emptied for another rule's rows kept the indices marked in
/// the last rule's — which then named rows of the new rule's, and rebuilt one of them for
/// nothing on the next redraw, taking the keyboard from a box that had just been clicked into
/// (found 2026-09-28).
template <typename Row>
class Repeater {
public:
    Repeater() : model_(std::make_shared<slint::VectorModel<Row>>()) {}

    /// What the window's repeater is bound to.
    [[nodiscard]] const std::shared_ptr<slint::VectorModel<Row>>& model() const noexcept {
        return model_;
    }

    /// Empties it, so the window throws its items away and builds new ones.
    void clear() {
        model_->clear();
        stale_.clear();
    }

    /// `staleRows`: whether any of the rows now in the model has to be built again rather than
    /// updated to `rows`. The indices are kept for `renew`.
    template <typename Changed>
    bool markStale(const std::vector<Row>& rows, Changed changed) {
        const std::vector<std::size_t> stale = staleRows(*model_, rows, changed);
        stale_.insert(stale_.end(), stale.begin(), stale.end());
        return !stale.empty();
    }

    /// `writeRows`: the model updated to `rows` in place.
    void write(const std::vector<Row>& rows) { writeRows(*model_, rows); }

    /// Builds the rows `markStale` found again, as new elements, and forgets them.
    void renew() { renewRows(*model_, stale_); }

private:
    std::shared_ptr<slint::VectorModel<Row>> model_;
    std::vector<std::size_t> stale_;
};

} // namespace takt4::ui
