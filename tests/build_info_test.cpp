#include "core/build_info.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <regex>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("build info reports every dependency", "[build_info]") {
    const takt4::BuildInfo info = takt4::buildInfo();

    CHECK(std::regex_match(info.version, std::regex(R"(\d+\.\d+\.\d+)")));
    CHECK_FALSE(info.platform.empty());
    CHECK_FALSE(info.compiler.empty());
    CHECK_THAT(info.portaudio, ContainsSubstring("PortAudio V19"));
    CHECK(info.r8brain == "7.5"); // pinned in cmake/deps.cmake; the header's R8B_VERSION must agree
    CHECK(info.rtmidi == "6.0.0");
    CHECK(info.nlohmannJson == "3.12.0");
    CHECK(info.rtneuralRevision.size() == 40);

    // Both come from the third_party/link submodule. The asio version is read from its
    // header, so bumping the submodule fails this test until TAKT4_LINK_VERSION in
    // cmake/deps.cmake and third_party/README.md are updated to match.
    CHECK(info.link == "4.0");
    CHECK(info.kohlhoffAsio == "1.36.0");
}

TEST_CASE("describe() renders one line per component", "[build_info]") {
    const std::string text = takt4::describe(takt4::buildInfo());

    CHECK_THAT(text, ContainsSubstring("takt4 "));
    CHECK_THAT(text, ContainsSubstring("PortAudio:"));
    CHECK_THAT(text, ContainsSubstring("r8brain-free:"));
    CHECK_THAT(text, ContainsSubstring("Ableton Link:"));
    CHECK_THAT(text, ContainsSubstring("RtMidi:"));
    CHECK_THAT(text, ContainsSubstring("RTNeural:"));
    CHECK_THAT(text, ContainsSubstring("nlohmann/json:"));
    CHECK_THAT(text, ContainsSubstring("Slint:"));
}
