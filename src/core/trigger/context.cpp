#include "core/trigger/context.hpp"

namespace takt4::trigger {

std::string_view labelOf(Intensity intensity) noexcept {
    switch (intensity) {
    case Intensity::Calm:
        return "calm";
    case Intensity::Normal:
        return "normal";
    case Intensity::Intense:
        return "intense";
    }
    return "";
}

} // namespace takt4::trigger
