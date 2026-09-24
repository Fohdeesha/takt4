#include "core/io/blob_file.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;

namespace {

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

} // namespace

TEST_CASE("the blob checksum is FNV-1a over 32 bits", "[io]") {
    // The published test vectors, which is what the Python that writes the blobs is checked
    // against too — a blob written by one and read by the other has to agree to the bit.
    CHECK(takt4::io::fnv1a32(bytesOf("")) == 0x811C9DC5u);
    CHECK(takt4::io::fnv1a32(bytesOf("a")) == 0xE40C292Cu);
    CHECK(takt4::io::fnv1a32(bytesOf("foobar")) == 0xBF9CF968u);
}

TEST_CASE("a file is read whole, or refused with its name", "[io]") {
    const takt4::test::TempDir dir;
    const std::filesystem::path path = dir.path() / "blob.bin";
    const std::string written("TAKT4\0\x01\xff tail", 13);
    {
        std::ofstream out(path, std::ios::binary);
        out.write(written.data(), static_cast<std::streamsize>(written.size()));
    }
    const std::vector<std::byte> read = takt4::io::readWholeFile(path);
    REQUIRE(read.size() == written.size());
    CHECK(std::equal(read.begin(), read.end(), bytesOf(written).begin()));

    const std::filesystem::path empty = dir.path() / "empty.bin";
    { std::ofstream out(empty, std::ios::binary); }
    CHECK(takt4::io::readWholeFile(empty).empty());

    try {
        (void)takt4::io::readWholeFile(dir.path() / "not there.bin");
        FAIL("a missing file was read");
    } catch (const std::runtime_error& error) {
        CHECK_THAT(error.what(), ContainsSubstring("not there.bin"));
        CHECK_THAT(error.what(), ContainsSubstring("cannot open"));
    }
}
