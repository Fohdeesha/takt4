#include "core/build_info.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

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
    CHECK(info.kissfft == "131.2.0"); // the tag pinned in cmake/deps.cmake

    // Both come from the third_party/link submodule. The asio version is read from its
    // header, so bumping the submodule fails this test until TAKT4_LINK_VERSION in
    // cmake/deps.cmake and third_party/README.md are updated to match.
    CHECK(info.link == "4.0");
    CHECK(info.kohlhoffAsio == "1.36.0");
}

TEST_CASE("a build names the commit it came from", "[build_info]") {
    // The audit: releases were built by hand from a working tree and `--version` carried no
    // commit and no dirty flag, so a binary could not say what it was built from.
    const takt4::BuildInfo info = takt4::buildInfo();
    CHECK_FALSE(info.commit.empty());
    CHECK(info.commit != "unknown"); // this tree is a git checkout, and git is there to ask
    CHECK_THAT(takt4::describe(info), ContainsSubstring("commit:        " + info.commit));

    // The bare version only for a clean build of its own release tag.
    takt4::BuildInfo release = info;
    release.version = "0.9.7";
    release.commit = "v0.9.7";
    CHECK(takt4::versionLabel(release) == "0.9.7");
    release.commit = "v0.9.7-dirty";
    CHECK(takt4::versionLabel(release) == "0.9.7 (v0.9.7-dirty)");
    release.commit = "v0.9.7-12-gfccd01d";
    CHECK(takt4::versionLabel(release) == "0.9.7 (v0.9.7-12-gfccd01d)");
}

TEST_CASE("the build does not need the newest C++ runtime to lock a mutex", "[build_info]") {
    // The audit's H17: built with Visual Studio 2022 17.10 or later, std::mutex's constructor
    // is constexpr, and an older msvcp140.dll on the machine crashes the program on its first
    // lock. The macro that turns that off has to be in every file built here (CMakeLists.txt).
    // Whether a machine with an old runtime then runs takt4 is not something a test here can
    // show; that the build asks for it is.
#if defined(_MSC_VER)
#if defined(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
    SUCCEED("_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR is defined");
#else
    FAIL("_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR is not defined for this build");
#endif
#else
    SKIP("only the Microsoft C++ runtime has this");
#endif
}

#if defined(_WIN32)
namespace {

/// One string from an executable's version resource, or empty.
std::wstring versionString(const std::filesystem::path& exe, const wchar_t* name) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (size == 0) {
        return {};
    }
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) {
        return {};
    }
    wchar_t* value = nullptr;
    UINT length = 0;
    const std::wstring key = std::wstring(L"\\StringFileInfo\\040904b0\\") + name;
    if (!VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&value), &length) ||
        value == nullptr) {
        return {};
    }
    return std::wstring(value, length > 0 ? length - 1 : 0);
}

std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end()); // version strings are ASCII
}

} // namespace

TEST_CASE("the executables say which release and which commit they are", "[build_info]") {
    // The audit: no version resource, so a crash report's module list and Explorer's
    // Properties said nothing about which build it was. The real executables beside this
    // test binary, read the way Windows reads them.
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    const std::filesystem::path bin = std::filesystem::path(self).parent_path();
    const takt4::BuildInfo info = takt4::buildInfo();
    int checked = 0;
    for (const wchar_t* name : {L"takt4-cli.exe", L"takt4.exe"}) {
        const std::filesystem::path exe = bin / name;
        if (!std::filesystem::exists(exe)) {
            continue; // a core-only build has no takt4.exe
        }
        INFO(exe.string());
        CHECK(versionString(exe, L"FileVersion") == widen(info.version));
        CHECK(versionString(exe, L"ProductVersion") == widen(info.commit));
        CHECK(versionString(exe, L"ProductName") == L"takt4");
        CHECK(versionString(exe, L"OriginalFilename") == name);
        ++checked;
    }
    CHECK(checked >= 1);
}
#endif

TEST_CASE("describe() renders one line per component", "[build_info]") {
    const std::string text = takt4::describe(takt4::buildInfo());

    CHECK_THAT(text, ContainsSubstring("takt4 "));
    CHECK_THAT(text, ContainsSubstring("PortAudio:"));
    CHECK_THAT(text, ContainsSubstring("r8brain-free:"));
    CHECK_THAT(text, ContainsSubstring("Ableton Link:"));
    CHECK_THAT(text, ContainsSubstring("RtMidi:"));
    CHECK_THAT(text, ContainsSubstring("RTNeural:"));
    CHECK_THAT(text, ContainsSubstring("KissFFT:"));
    CHECK_THAT(text, ContainsSubstring("nlohmann/json:"));
    CHECK_THAT(text, ContainsSubstring("Slint:"));
}
