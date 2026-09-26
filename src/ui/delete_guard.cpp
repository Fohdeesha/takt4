#include "ui/delete_guard.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace takt4::ui {

bool DeleteGuard::press(int index, Clock::time_point now) noexcept {
    const bool second = last_ && index == lastIndex_ && now - *last_ < interval();
    last_ = now;
    lastIndex_ = index;
    return !second;
}

DeleteGuard::Clock::duration DeleteGuard::interval() noexcept {
#if defined(_WIN32)
    return std::chrono::milliseconds{::GetDoubleClickTime()};
#else
    return std::chrono::milliseconds{500};
#endif
}

} // namespace takt4::ui
