#include "ui/headless.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace takt4::ui {
namespace {

/// Every adapter made and not yet destroyed. The platform is only ever used from the thread that
/// runs the windows, so this is never touched from two at once.
std::vector<HeadlessWindow*>& made() {
    static std::vector<HeadlessWindow*> adapters;
    return adapters;
}

/// The platform the runtime is given instead of winit. It never runs an event loop:
/// whoever installed it either renders on demand (`takt4-shot`) or steps the timers
/// itself (`takt4_ui_tests`).
class HeadlessPlatform final : public slint::platform::Platform {
public:
    explicit HeadlessPlatform(slint::PhysicalSize size) : size_(size) {}

    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override {
        auto adapter = std::make_unique<HeadlessWindow>(size_);
        window = adapter.get();
        return adapter;
    }

    /// The most recent adapter the runtime asked for. Owned by the runtime, not by this.
    HeadlessWindow* window = nullptr;

private:
    slint::PhysicalSize size_;
};

} // namespace

HeadlessWindow::HeadlessWindow(slint::PhysicalSize size)
    : size_(size), renderer_(slint::platform::SoftwareRenderer::RepaintBufferType::NewBuffer) {
    made().push_back(this);
}

HeadlessWindow::~HeadlessWindow() {
    std::erase(made(), this);
}

HeadlessWindow* const* installHeadlessPlatform(std::uint32_t width, std::uint32_t height) {
    auto platform = std::make_unique<HeadlessPlatform>(slint::PhysicalSize({width, height}));
    // The address of the member outlives this function: the platform is handed to the
    // runtime, which keeps it for the life of the process. A pointer-to-pointer rather
    // than a pointer because the adapter does not exist yet — the runtime only asks for
    // one when the first component is created.
    HeadlessWindow* const* latest = &platform->window;
    slint::platform::set_platform(std::move(platform));
    return latest;
}

HeadlessWindow* headlessAdapterFor(const slint::Window& window) {
    // A `slint::Window` is one reference-counted handle to its adapter, and an adapter's own
    // `window()` is that same handle — slint-platform.h reinterprets one as the other. Two that
    // hold the same bytes are one window.
    for (HeadlessWindow* adapter : made()) {
        if (std::memcmp(&adapter->window(), &window, sizeof(slint::Window)) == 0) {
            return adapter;
        }
    }
    return nullptr;
}

} // namespace takt4::ui
