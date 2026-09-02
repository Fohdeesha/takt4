#include "core/build_info.hpp"
#include "ui/app.hpp"

#include <iostream>
#include <string_view>

namespace {

void printUsage(std::ostream& out) {
    out << "usage: takt4 [--version] [--help]\n"
           "\n"
           "  --version, -v   print version and library information, then exit\n"
           "  --help, -h      print this help, then exit\n";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            std::cout << takt4::describe(takt4::buildInfo());
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
        std::cerr << "takt4: unknown option '" << arg << "'\n";
        printUsage(std::cerr);
        return 2;
    }

    return takt4::ui::run();
}
