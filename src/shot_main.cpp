// takt4-shot — render the main window to an image without a display. See src/ui/shot.cpp
// for why this exists. A bench tool, like takt4-cli; never packaged.

#include "core/build_info.hpp"
#include "ui/app.hpp"
#include "ui/window_state.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void printUsage(std::ostream& out) {
    out << "takt4-shot " << takt4::buildInfo().version
        << " — render the main window to a BMP\n"
           "\n"
           "  takt4-shot OUT.bmp [--size WxH] [--scale S] [--stopped | --trouble]\n"
           "                     [--mockup] [--fold inputs,outputs] [--panicked]\n"
           "                     [--rules [--dmx] [--state NAME] | --fixtures | --about |\n"
           "                      --widgets]\n"
           "\n"
           "  --size WxH      the window size to render, default the window's own\n"
           "  --scale S       the display's scale, 1.25 for 125 %: the size stays\n"
           "                  logical and the picture is S times larger\n"
           "  --stopped       draw the idle window — blank readouts and the manual\n"
           "                  controls disabled — instead of a tracker running\n"
           "  --trouble       the running window with outputs and control inputs that\n"
           "                  cannot be reached, each saying why under itself\n"
           "  --mockup        the running window holding exactly what the approved mockup\n"
           "                  of 2026-09-29 holds (design/weltformat-dark), to lay the two\n"
           "                  pictures over each other\n"
           "  --fold WHICH    draw inputs, outputs or both (inputs,outputs) folded\n"
           "  --panicked      draw PANIC engaged, with RELEASE beside it\n"
           "  --rules         draw §5.9's rule editor instead of the main window\n"
           "  --dmx           with --rules, its lighting half rather than the OSC one\n"
           "  --state NAME    with --rules, another of its states: message, onset, beat,\n"
           "                  backbeat, log, fit, folded, b-on, b-off, none, no-lights, and\n"
           "                  the Liberation prompt: liberation, liberation-4,\n"
           "                  liberation-clash, liberation-warning; with --fixtures: par,\n"
           "                  none, message; and\n"
           "                  for the main window: halved, apart, frozen\n"
           "  --fixtures      draw the lighting patch editor instead of the main window\n"
           "  --about         draw the About box instead of the main window\n"
           "  --widgets       draw one of each of the main window's controls, in each state\n"
           "\n"
           "Draws the real component with Slint's software renderer, so it needs no\n"
           "display: the readouts are filled by running the tracker over\n"
           "tests/data/features/synthetic.wav with the generic weights, which\n"
           "`takt4-cli track tests/data/features/synthetic.wav --weights generic` prints as\n"
           "\"997 frames, 22 beats (6 downbeats), ending at 128.3 BPM in 4/4, locked\".\n";
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path out;
    takt4::ui::ShotOptions options;
    int width = options.width;
    int height = options.height;
    bool sized = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
        if (arg == "--stopped") {
            options.running = false;
        } else if (arg == "--rules") {
            options.rules = true;
        } else if (arg == "--fixtures") {
            options.fixtures = true;
        } else if (arg == "--about") {
            options.about = true;
        } else if (arg == "--widgets") {
            options.widgets = true;
        } else if (arg == "--dmx") {
            options.dmx = true;
        } else if (arg == "--trouble") {
            options.trouble = true;
        } else if (arg == "--mockup") {
            options.mockup = true;
        } else if (arg == "--panicked") {
            options.panicked = true;
        } else if (arg == "--state") {
            if (i + 1 >= argc) {
                std::cerr << "takt4-shot: --state needs a name; see --help\n";
                return 2;
            }
            options.state = argv[++i];
        } else if (arg == "--fold") {
            if (i + 1 >= argc) {
                std::cerr << "takt4-shot: --fold needs inputs, outputs or inputs,outputs\n";
                return 2;
            }
            const std::string which = argv[++i];
            options.foldInputs = which.find("inputs") != std::string::npos;
            options.foldOutputs = which.find("outputs") != std::string::npos;
            if (!options.foldInputs && !options.foldOutputs) {
                std::cerr << "takt4-shot: --fold takes inputs, outputs or inputs,outputs\n";
                return 2;
            }
        } else if (arg == "--scale") {
            if (i + 1 >= argc) {
                std::cerr << "takt4-shot: --scale needs a number, 1.25 for 125 %\n";
                return 2;
            }
            try {
                options.scale = std::stof(argv[++i]);
            } catch (const std::exception&) {
                options.scale = 0.0f;
            }
            if (!(options.scale >= 0.5f && options.scale <= 4.0f)) {
                std::cerr << "takt4-shot: --scale is 0.5 to 4\n";
                return 2;
            }
        } else if (arg == "--size") {
            sized = true;
            if (i + 1 >= argc) {
                std::cerr << "takt4-shot: --size needs WxH\n";
                return 2;
            }
            const std::string value = argv[++i];
            const std::size_t cross = value.find('x');
            if (cross == std::string::npos) {
                std::cerr << "takt4-shot: --size expects WxH, got " << value << '\n';
                return 2;
            }
            try {
                width = std::stoi(value.substr(0, cross));
                height = std::stoi(value.substr(cross + 1));
            } catch (const std::exception&) {
                std::cerr << "takt4-shot: --size expects two numbers, got " << value << '\n';
                return 2;
            }
        } else if (arg.starts_with("-")) {
            std::cerr << "takt4-shot: unknown option '" << arg << "'\n";
            printUsage(std::cerr);
            return 2;
        } else if (out.empty()) {
            out = std::filesystem::path(arg);
        } else {
            std::cerr << "takt4-shot: one output file, not two\n";
            return 2;
        }
    }

    if (out.empty()) {
        printUsage(std::cerr);
        return 2;
    }

    // Each window renders at its own preferred size unless one was asked for, so a picture
    // is the layout as designed rather than the layout squeezed.
    if (options.rules && !sized) {
        width = takt4::ui::kRulesShotWidth;
        height = takt4::ui::kRulesShotHeight;
    }
    if (options.widgets && !sized) {
        width = 900;
        height = 720;
    }
    if (options.fixtures && !sized) {
        width = takt4::ui::kFixturesShotWidth;
        height = takt4::ui::kFixturesShotHeight;
    }
    if (options.about && !sized) {
        width = static_cast<int>(takt4::ui::kAboutWindowWidth);
        height = static_cast<int>(takt4::ui::kAboutWindowHeight);
    }
    options.width = width;
    options.height = height;
    try {
        return takt4::ui::renderShot(out, options);
    } catch (const std::exception& e) {
        std::cerr << "takt4-shot: " << e.what() << '\n';
        return 1;
    }
}
