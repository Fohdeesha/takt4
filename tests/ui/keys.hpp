#pragma once

// Keys a test presses in a text box, as the system it runs on has them.

#include <slint.h>

namespace takt4::tests {

/// To the end of the text in the box being typed in: End on Windows and Linux; Command+Right on a
/// Mac, whose text boxes leave End to the window as its own text fields do (the first macOS runs,
/// 2026-10-08). Slint gives Command as its `control`.
inline void endOfText(slint::Window& window) {
#if defined(__APPLE__)
    window.dispatch_key_press_event(slint::SharedString("\x11"));
    window.dispatch_key_press_event(slint::SharedString("\xEF\x9C\x83")); // Key.RightArrow
    window.dispatch_key_release_event(slint::SharedString("\xEF\x9C\x83"));
    window.dispatch_key_release_event(slint::SharedString("\x11"));
#else
    window.dispatch_key_press_event(slint::SharedString("\xEF\x9C\xAB")); // Key.End, U+F72B
    window.dispatch_key_release_event(slint::SharedString("\xEF\x9C\xAB"));
#endif
}

} // namespace takt4::tests
