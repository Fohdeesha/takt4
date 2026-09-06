// takt4-shot — render the main window to an image without a display. See src/ui/shot.cpp
// for why this exists. A bench tool, like takt4-cli; never packaged.

#include "core/build_info.hpp"
#include "ui/app.hpp"

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
           "  takt4-shot OUT.bmp [--size WxH] [--stopped] [--rules]\n"
           "\n"
           "  --size WxH      the window size to render, default the window's own\n"
           "  --stopped       draw the idle window — blank readouts and the manual\n"
           "                  controls disabled — instead of a tracker running\n"
           "  --rules         draw §5.9's rule editor instead of the main window\n"
           "\n"
           "Draws the real component with Slint's software renderer, so it needs no\n"
           "display: the readouts are filled by running the tracker over\n"
           "tests/data/features/synthetic.wav, which `takt4-cli track` prints as\n"
           "\"499 frames, 21 beats (5 downbeats), ending at 128.4 BPM in 4/4, locked\".\n";
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
    options.width = width;
    options.height = height;
    try {
        return takt4::ui::renderShot(out, options);
    } catch (const std::exception& e) {
        std::cerr << "takt4-shot: " << e.what() << '\n';
        return 1;
    }
}
