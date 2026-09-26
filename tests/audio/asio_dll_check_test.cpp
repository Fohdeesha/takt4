#if defined(_WIN32)

#include "core/audio/asio_dll_check.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

// The check the patched ASIO SDK makes before it offers a driver (cmake/asiosdk.cmake): false only
// for a DLL Windows says is not there. What `OpenFile` got wrong — the audit of 2026-09-25, M9 —
// was a path of 127 bytes or more and the `%SystemRoot%\...` of a REG_EXPAND_SZ value, both of
// which COM loads, and both of which hid a working driver.

namespace {

bool present(const std::string& path) {
    return takt4::audio::asioDriverDllPresent(path.c_str(), path.size() + 1);
}

} // namespace

TEST_CASE("an ASIO driver's DLL is missing only when Windows says it is not there", "[audio]") {
    const takt4::test::TempDir folder;
    const std::filesystem::path dll = folder.path() / "driver.dll";
    std::ofstream(dll) << "not really a DLL";
    REQUIRE(std::filesystem::exists(dll));
    CHECK(present(dll.string()));

    SECTION("a long path, which OpenFile refuses at 127 bytes") {
        std::filesystem::path deep = folder.path();
        while (deep.string().size() < 160) {
            deep /= "a-folder-with-a-long-name";
        }
        std::filesystem::create_directories(deep);
        const std::filesystem::path distant = deep / "driver.dll";
        std::ofstream(distant) << "not really a DLL";
        REQUIRE(distant.string().size() > 127);
        CHECK(present(distant.string()));
    }

    SECTION("an unexpanded REG_EXPAND_SZ path") {
        CHECK(present("%SystemRoot%\\System32\\kernel32.dll"));
        CHECK_FALSE(present("%SystemRoot%\\System32\\takt4-no-such-driver.dll"));
    }

    SECTION("a path an installer put quotes round") {
        CHECK(present("\"" + dll.string() + "\""));
    }

    SECTION("a registry value that fills its buffer with no terminator") {
        const std::string path = dll.string();
        std::array<char, 512> buffer{};
        buffer.fill('x');
        std::memcpy(buffer.data(), path.data(), path.size());
        // Only `path.size()` bytes are the value; the rest of the buffer is not read.
        CHECK(takt4::audio::asioDriverDllPresent(buffer.data(), path.size()));
    }

    SECTION("a DLL that is gone, or a folder that is") {
        CHECK_FALSE(present((folder.path() / "gone.dll").string()));
        CHECK_FALSE(present((folder.path() / "no-such-folder" / "driver.dll").string()));
    }
}

#endif
