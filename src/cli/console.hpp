#pragma once

// What the console's commands share: single keys from the terminal, and the rule that
// turns a weight set's name into a file. Header-only, so `annotate` and `track` read the
// keyboard the same way without either owning the class.

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace takt4::cli {

/// A bare name is one of the committed sets; anything with a separator or a suffix is
/// taken as a path, so a set converted somewhere else can be tried without a rebuild.
inline std::filesystem::path resolveWeights(const std::string& spec) {
    const std::filesystem::path given(spec);
    if (given.has_parent_path() || given.has_extension()) {
        return given;
    }
    return std::filesystem::path(TAKT4_WEIGHTS_DIR) / (spec + ".bin");
}

/// Single keys from the terminal, without waiting for one.
///
/// §5.5's manual controls — tap, the octave shift, the downbeat snap, the latency slider
/// — need somewhere to be pressed before there is a UI, and `BeatEngine`'s control queue
/// needs a producer that is not a test. This is both, and it is the shape a UI's input
/// handling takes too: read a key, post a command, never touch the tracker.
///
/// It does nothing at all unless stdin is something keys can actually come from. CI runs
/// this binary without a console, and a redirected stdin must never be left in a mode
/// nobody puts back.
class KeyReader {
public:
    KeyReader() {
#if defined(_WIN32)
        // Not `_isatty`: that is true of any character device, and NUL is one — measured,
        // a run with stdin redirected from /dev/null was called interactive. Only a real
        // console input handle has a console mode, and a console input handle is the only
        // thing _kbhit reads from anyway.
        DWORD mode = 0;
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        interactive_ =
            in != nullptr && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode) != 0;
#else
        interactive_ = isatty(STDIN_FILENO) != 0;
        if (!interactive_) {
            return;
        }
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
            interactive_ = false;
            return;
        }
        struct termios raw = saved_;
        // Keys as they are pressed rather than lines, and not echoed back in among the
        // beats. ISIG is left alone, so Ctrl+C still reaches the signal handler.
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        // VMIN 0 with VTIME 0 is what makes read() return at once with whatever is there,
        // rather than blocking the loop that has a MIDI clock to tick.
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            interactive_ = false;
            return;
        }
        restore_ = true;
#endif
    }

    ~KeyReader() {
#if !defined(_WIN32)
        if (restore_) {
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        }
#endif
    }

    KeyReader(const KeyReader&) = delete;
    KeyReader& operator=(const KeyReader&) = delete;

    bool interactive() const noexcept { return interactive_; }

    /// The next key waiting, or 0 when none is.
    int poll() noexcept {
        if (!interactive_) {
            return 0;
        }
#if defined(_WIN32)
        if (_kbhit() == 0) {
            return 0;
        }
        const int key = _getch();
        // An arrow or function key arrives as a 0 or 0xE0 prefix and then its scan code.
        // Both bytes are already buffered; leaving the second would hand back a scan code
        // that collides with a letter on the next poll.
        if (key == 0 || key == 0xE0) {
            (void)_getch();
            return 0;
        }
        return key;
#else
        char pressed = 0;
        return read(STDIN_FILENO, &pressed, 1) == 1 ? static_cast<unsigned char>(pressed) : 0;
#endif
    }

private:
    bool interactive_ = false;
#if !defined(_WIN32)
    struct termios saved_{};
    bool restore_ = false;
#endif
};

} // namespace takt4::cli
