#pragma once

// What the console's commands share: single keys from the terminal, Ctrl+C as a flag rather
// than the end of the process, and the rule that turns a weight set's name into a file.
// Header-only, so `annotate` and `track` read the keyboard the same way without either
// owning the class.

#include "core/model/weights.hpp"
#include "core/tracking/state_space.hpp"

#if TAKT4_CLI_EMBEDDED_ASSETS
#include "core/assets/embedded.hpp"
#endif

#include <atomic>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

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

/// A name under assets/statespace/ — "default", "fps100" — or a path to a blob.
inline std::filesystem::path resolveStateSpace(const std::string& which) {
    const std::filesystem::path given(which);
    if (given.has_extension() || given.has_parent_path()) {
        return given;
    }
    return std::filesystem::path(TAKT4_STATESPACE_DIR) / (which + ".bin");
}

/// The weight set the application builds in, by the name the console knows it by.
inline constexpr const char* kBuiltInWeights = "electronic";

/// The weights `spec` names. **The built-in set comes from this binary** wherever it has
/// them (every full build, which is what ships): the console is attached to every release
/// beside the app, and it used to open its weights from the source tree it was built in, so
/// on any other machine `beats`, `track` and `annotate` failed (the audit's H19). Any other
/// set is a file — under the source tree for a bare name, which only a bench has.
inline model::ModelWeights loadWeights(const std::string& spec) {
#if TAKT4_CLI_EMBEDDED_ASSETS
    if (spec == kBuiltInWeights) {
        return model::ModelWeights::fromBytes(assets::weights(), "electronic (built in)");
    }
#endif
    const std::filesystem::path path = resolveWeights(spec);
    std::error_code code;
    if (path == std::filesystem::path(TAKT4_WEIGHTS_DIR) / (spec + ".bin") &&
        !std::filesystem::exists(path, code)) {
        // A bare name that is not the built-in set, on a machine without the source tree —
        // said as what it is, rather than as a path the operator never typed.
        throw std::runtime_error("no weight set called \"" + spec + "\" here: this console has \"" +
                                 std::string(kBuiltInWeights) +
                                 "\" built in, and takes any other set as a path to its .bin");
    }
    return model::ModelWeights::fromFile(path);
}

/// The state space `which` names; "default" from this binary wherever it has it, as above.
inline tracking::StateSpaceModel loadStateSpace(const std::string& which) {
#if TAKT4_CLI_EMBEDDED_ASSETS
    if (which == "default") {
        return tracking::StateSpaceModel::fromBytes(assets::stateSpace(), "default (built in)");
    }
#endif
    return tracking::StateSpaceModel::fromFile(resolveStateSpace(which));
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

/// Ctrl+C, and a request to terminate, as a flag the loop can look at — for as long as this
/// lives — instead of the end of the process. `annotate` had nothing of the kind: a Ctrl+C in
/// the middle of tapping along killed it and every tap with it, because a key reader never
/// sees Ctrl+C at all (`_getch` cannot read it, and the terminal's ISIG turns it into a signal
/// on the other platforms), and the default handler for the signal ends the process (the
/// audit's Low items). The handlers in force before are put back after.
class Interrupts {
public:
    Interrupts() noexcept {
        flag().store(false);
        previousInt_ = std::signal(SIGINT, &Interrupts::raise);
        previousTerm_ = std::signal(SIGTERM, &Interrupts::raise);
    }
    ~Interrupts() {
        (void)std::signal(SIGINT, previousInt_);
        (void)std::signal(SIGTERM, previousTerm_);
    }
    Interrupts(const Interrupts&) = delete;
    Interrupts& operator=(const Interrupts&) = delete;

    /// Whether one has arrived since this was made.
    static bool requested() noexcept { return flag().load(); }

private:
    using Handler = void (*)(int);
    /// A lock-free atomic, and first touched by the constructor, so a signal handler only
    /// ever stores to one that already exists — the one thing a handler may safely do.
    static std::atomic<bool>& flag() noexcept {
        static std::atomic<bool> raised{false};
        return raised;
    }
    /// **Installed again first.** Windows' runtime puts a signal back to its default before
    /// it calls the handler, so the second Ctrl+C of an impatient operator ended the process —
    /// before annotate's taps were written, or `track --device` sent the MIDI clock its Stop
    /// (the audit of 2026-09-25, L47).
    static void raise(int signal) {
        (void)std::signal(signal, &Interrupts::raise);
        flag().store(true);
    }

    Handler previousInt_ = SIG_DFL;
    Handler previousTerm_ = SIG_DFL;
};

} // namespace takt4::cli
