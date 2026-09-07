#pragma once

#include <slint.h>

#include <cstddef>

namespace takt4::tests {

/// Counts what a model tells the views bound to it.
///
/// A Slint repeater is one of these underneath, and it responds to each notification
/// differently: `row_changed` updates the item it already has, while `reset` throws every
/// item away and builds new ones. That difference is the whole of what these tests watch
/// for — a rebuilt item is a fresh element tree, so a `LineEdit` that had the keyboard loses
/// it mid-word and a `Slider` being dragged loses the grab on the first pixel of movement.
///
/// Attach with `model->attach_peer(watch)`, holding the `shared_ptr` for as long as the
/// counts matter: the model keeps a weak reference and quietly drops a listener that has
/// gone, so a watch that is not held counts nothing rather than failing.
struct ModelWatch : slint::private_api::ModelChangeListener {
    int resets = 0;
    int changes = 0;
    int added = 0;
    int removed = 0;

    void row_added(std::size_t, std::size_t count) override { added += static_cast<int>(count); }
    void row_removed(std::size_t, std::size_t count) override {
        removed += static_cast<int>(count);
    }
    void row_changed(std::size_t) override { ++changes; }
    void reset() override { ++resets; }
};

} // namespace takt4::tests
