#pragma once

// Rendering a window from inside the test suite, so a layout can be asserted on rather
// than described.
//
// The window tests drive callbacks and read properties, which says nothing about where
// anything ended up on screen — and "the bottom of the window is cut off when it is short"
// is a bug that no property can report. `takt4-shot` renders the same component with the
// same software renderer; this is that, in a test.
//
// Slint permits one platform per process and it has to be installed before any component
// exists, so `main()` installs it and leaves the adapter here for a test to find.

#include "ui/headless.hpp"

#include <slint.h>

#include <cstddef>
#include <vector>

namespace takt4::tests {

/// Where `main()` leaves the headless platform's adapter slot. Null until it does; the
/// adapter behind it is null until the runtime asks for a window, which it does when the
/// first component is created.
inline takt4::ui::HeadlessWindow* const*& headlessPlatform() {
    static takt4::ui::HeadlessWindow* const* slot = nullptr;
    return slot;
}

/// One rendered window, and the pixels in it.
struct Shot {
    int width = 0;
    int height = 0;
    std::vector<slint::Rgb8Pixel> pixels;

    /// x from the left, y from the top, as a person reads the picture.
    slint::Rgb8Pixel at(int x, int y) const {
        return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                      static_cast<std::size_t>(x)];
    }

    bool is(int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) const {
        const slint::Rgb8Pixel p = at(x, y);
        return p.r == r && p.g == g && p.b == b;
    }
};

/// Renders `window` at `width` x `height`. The sequence is `takt4-shot`'s: show() creates
/// the adapter, and the two dispatches give the scene its scale and size, which nothing
/// else would do with no window manager to hear from.
template <typename Window>
Shot render(Window& window, int width, int height) {
    window.show();
    takt4::ui::HeadlessWindow* const adapter = *headlessPlatform();
    adapter->resize(slint::PhysicalSize(
        {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}));
    window.window().dispatch_scale_factor_change_event(1.0f);
    window.window().dispatch_resize_event(
        slint::LogicalSize({static_cast<float>(width), static_cast<float>(height)}));

    Shot shot;
    shot.width = width;
    shot.height = height;
    shot.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    adapter->software().render(shot.pixels, static_cast<std::size_t>(width));
    return shot;
}

} // namespace takt4::tests
