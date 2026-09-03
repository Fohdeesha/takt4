#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace takt4::test {

/// A fresh directory under the system temp directory, removed with everything in it
/// when the object goes away.
class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::uniform_int_distribution<unsigned> hex(0, 15);
        std::string suffix;
        for (int i = 0; i < 12; ++i) {
            suffix += "0123456789abcdef"[hex(device)];
        }
        path_ = std::filesystem::temp_directory_path() / ("takt4-test-" + suffix);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return path_; }
    std::filesystem::path file(std::string_view name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

} // namespace takt4::test
