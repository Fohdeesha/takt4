#pragma once

#include <slint-platform.h>

#include <cstdint>
#include <optional>
#include <string>

namespace takt4::ui {

/// A window that is never shown: it reports a fixed size and owns a software renderer.
class HeadlessWindow final : public slint::platform::WindowAdapter {
public:
    explicit HeadlessWindow(slint::PhysicalSize size);
    ~HeadlessWindow() override;
    HeadlessWindow(const HeadlessWindow&) = delete;
    HeadlessWindow& operator=(const HeadlessWindow&) = delete;

    slint::platform::AbstractRenderer& renderer() override { return renderer_; }
    slint::PhysicalSize size() override { return size_; }

    /// What the program last asked the window to be — `Window::set_size`, which the window
    /// controller calls when it opens and on every fold. Kept rather than obeyed: nothing here is
    /// a window manager, and the size the adapter reports is the one a test lays the window out
    /// at (and places popups inside). A test reads this to see what a fold asked for.
    void set_size(slint::PhysicalSize size) override { requested_ = size; }
    std::optional<slint::PhysicalSize> requested() const noexcept { return requested_; }

    /// What the adapter reports from here on. Slint permits one platform per process, and
    /// the platform fixes this size when it builds the adapter — so without a way to change
    /// it afterwards, a test could only ever see the window at one size. Rendering the same
    /// window at several heights is the only way to test that a short one is not cut off.
    /// Dispatch a resize event to the window as well; this half only moves the buffer.
    void resize(slint::PhysicalSize size) noexcept { size_ = size; }

    /// Draws into a buffer of the caller's. `takt4-shot` writes that out as an image;
    /// a test that only needs the window to exist never calls it.
    slint::platform::SoftwareRenderer& software() noexcept { return renderer_; }

private:
    slint::PhysicalSize size_;
    std::optional<slint::PhysicalSize> requested_;
    slint::platform::SoftwareRenderer renderer_;
};

/// Installs a Slint platform with no window system behind it, and returns the adapter the
/// runtime will be given.
///
/// This is what lets takt4's window be *worked on* from a machine with no display — and
/// there is no display in the sessions takt4 is developed from, nor on a CI runner. It
/// backs two things: `takt4-shot`, which renders the window to an image (HANDOFF §6), and
/// `takt4_ui_tests`, which drives the window's callbacks and its redraw timer without one
/// ever reaching a screen.
///
/// Slint's own `slint::testing::init()` would also do this, and would additionally let a test
/// *find* an element by name. It is behind `SLINT_FEATURE_EXPERIMENTAL`, which this build
/// deliberately leaves off; everything here is stable API.
///
/// **What that does not cost is driving the window.** `dispatch_pointer_press_event`,
/// `dispatch_key_press_event` and the rest are stable, and they work against this platform —
/// so a test can click into a text box, type into it and tab away, which is the only way to
/// test behaviour that lives in the markup rather than in a controller.
///
/// **Call once, before any Slint component is created** — Slint permits one platform per
/// process and creating a component installs the default one. The returned pointer is
/// owned by the runtime and stays valid for the life of the process; it is null until the
/// runtime asks for a window, which it does when the first component is created.
HeadlessWindow* const* installHeadlessPlatform(std::uint32_t width, std::uint32_t height);

/// The adapter behind `window`, among every one the headless platform has made and not yet
/// destroyed — or null. The most recent adapter is not always the one wanted: a window
/// controller makes the rule editor, the patch editor and the About box after its main window,
/// so a test rendering the main window, or reading the size it asked for, has to find its own.
HeadlessWindow* headlessAdapterFor(const slint::Window& window);

/// What was last copied to the headless platform's clipboard (Ctrl+C in a text box), or empty.
std::string headlessClipboard();

} // namespace takt4::ui
