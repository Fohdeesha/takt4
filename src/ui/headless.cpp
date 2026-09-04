#include "ui/headless.hpp"

#include <memory>

namespace takt4::ui {
namespace {

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

} // namespace takt4::ui
