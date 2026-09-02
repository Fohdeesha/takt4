#include "ui/app.hpp"

#include "core/build_info.hpp"

#include "main_window.h" // generated from main_window.slint

#include <string>

namespace takt4::ui {

int run() {
    auto window = MainWindow::create();
    const std::string status = "takt4 " + buildInfo().version;
    window->set_status_text(slint::SharedString(status));
    window->run();
    return 0;
}

} // namespace takt4::ui
