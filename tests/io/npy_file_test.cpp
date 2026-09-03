#include "core/io/npy_file.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::io::NpyMatrix;
using takt4::io::readNpyFloat32;
using takt4::io::writeNpyFloat32;

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void spit(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::string floatBytes(std::initializer_list<float> values) {
    std::string out;
    for (const float v : values) {
        char bytes[sizeof v];
        std::memcpy(bytes, &v, sizeof v);
        out.append(bytes, sizeof v);
    }
    return out;
}

// A .npy file with the given version and header text, numpy's framing around it.
std::string npyFile(unsigned char major, std::string header, const std::string& data) {
    std::string out = "\x93NUMPY";
    out.push_back(static_cast<char>(major));
    out.push_back('\0');
    header.push_back('\n');
    if (major == 1) {
        out.push_back(static_cast<char>(header.size() & 0xFF));
        out.push_back(static_cast<char>(header.size() >> 8));
    } else {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<char>((header.size() >> shift) & 0xFF));
        }
    }
    return out + header + data;
}

} // namespace

TEST_CASE("npy round trip", "[io]") {
    takt4::test::TempDir dir;
    const std::vector<float> values = {1.5f, -2.0f, 0.0f, 3.25f, 1e-7f, -1e7f};
    writeNpyFloat32(dir.file("m.npy"), 2, 3, values);

    const NpyMatrix back = readNpyFloat32(dir.file("m.npy"));
    CHECK(back.rows == 2);
    CHECK(back.cols == 3);
    CHECK(back.values == values);
    CHECK(back.at(1, 0) == 3.25f);

    SECTION("the file is what numpy writes: version 1.0, a 64-byte-aligned header") {
        // numpy 2.5.2 writes exactly these 152 bytes for this array (np.save to a BytesIO).
        const std::string bytes = slurp(dir.file("m.npy"));
        REQUIRE(bytes.size() == 128 + values.size() * sizeof(float));
        CHECK(bytes.substr(0, 6) == "\x93NUMPY");
        CHECK(bytes[6] == 1);
        CHECK(bytes[7] == 0);
        const auto headerLength = static_cast<std::size_t>(
            static_cast<unsigned char>(bytes[8]) | (static_cast<unsigned char>(bytes[9]) << 8));
        CHECK(headerLength == 118);
        const std::string header = bytes.substr(10, headerLength);
        const std::string dict = "{'descr': '<f4', 'fortran_order': False, 'shape': (2, 3), }";
        CHECK(header.substr(0, dict.size()) == dict);
        CHECK(header.substr(dict.size(), headerLength - dict.size() - 1) == std::string(58, ' '));
        CHECK(header.back() == '\n');
        CHECK(bytes.substr(128) == floatBytes({1.5f, -2.0f, 0.0f, 3.25f, 1e-7f, -1e7f}));
    }

    SECTION("empty and skinny shapes") {
        writeNpyFloat32(dir.file("empty.npy"), 0, 288, {});
        const NpyMatrix empty = readNpyFloat32(dir.file("empty.npy"));
        CHECK(empty.rows == 0);
        CHECK(empty.cols == 288);
        CHECK(empty.values.empty());

        const std::vector<float> column = {1.0f, 2.0f, 3.0f};
        writeNpyFloat32(dir.file("column.npy"), 3, 1, column);
        CHECK(readNpyFloat32(dir.file("column.npy")).values == column);
    }

    SECTION("a size that does not match the shape is refused") {
        CHECK_THROWS_AS(writeNpyFloat32(dir.file("bad.npy"), 2, 2, values), std::invalid_argument);
    }
}

TEST_CASE("readNpyFloat32() accepts the header variants numpy produces", "[io]") {
    takt4::test::TempDir dir;
    const std::string data = floatBytes({1.0f, 2.0f, 3.0f, 4.0f});

    SECTION("version 2.0, four-byte header length") {
        spit(dir.file("v2.npy"),
             npyFile(2, "{'descr': '<f4', 'fortran_order': False, 'shape': (2, 2), }", data));
        const NpyMatrix m = readNpyFloat32(dir.file("v2.npy"));
        CHECK(m.rows == 2);
        CHECK(m.cols == 2);
        CHECK(m.at(1, 1) == 4.0f);
    }
    SECTION("version 3.0") {
        spit(dir.file("v3.npy"),
             npyFile(3, "{'descr': '<f4', 'fortran_order': False, 'shape': (1, 4), }", data));
        CHECK(readNpyFloat32(dir.file("v3.npy")).cols == 4);
    }
    SECTION("double quotes, other key order, no padding, no trailing comma") {
        spit(dir.file("q.npy"),
             npyFile(1, "{\"shape\": (4, 1), \"fortran_order\": False, \"descr\": \"<f4\"}", data));
        const NpyMatrix m = readNpyFloat32(dir.file("q.npy"));
        CHECK(m.rows == 4);
        CHECK(m.cols == 1);
    }
}

TEST_CASE("readNpyFloat32() refuses what it cannot read, and says why", "[io]") {
    takt4::test::TempDir dir;
    const std::string data = floatBytes({1.0f, 2.0f, 3.0f, 4.0f});
    const auto check = [&](const std::string& name, const std::string& file,
                           const std::string& message) {
        spit(dir.file(name), file);
        CHECK_THROWS_WITH(readNpyFloat32(dir.file(name)), ContainsSubstring(message));
    };

    check("magic.npy", "NUMPY\x93" + std::string(64, ' '), "not a .npy file");
    check("v4.npy", npyFile(4, "{'descr': '<f4', 'fortran_order': False, 'shape': (2, 2), }", data),
          "version 4 is not supported");
    check("f8.npy", npyFile(1, "{'descr': '<f8', 'fortran_order': False, 'shape': (2, 2), }", data),
          "<f8");
    check("big-endian.npy",
          npyFile(1, "{'descr': '>f4', 'fortran_order': False, 'shape': (2, 2), }", data), ">f4");
    check("fortran.npy",
          npyFile(1, "{'descr': '<f4', 'fortran_order': True, 'shape': (2, 2), }", data),
          "Fortran order");
    check("1d.npy", npyFile(1, "{'descr': '<f4', 'fortran_order': False, 'shape': (4,), }", data),
          "expected a 2-D");
    check("3d.npy",
          npyFile(1, "{'descr': '<f4', 'fortran_order': False, 'shape': (1, 2, 2), }", data),
          "expected a 2-D");
    check("short.npy",
          npyFile(1, "{'descr': '<f4', 'fortran_order': False, 'shape': (2, 3), }", data),
          "shorter than its shape says");
    check("long.npy",
          npyFile(1, "{'descr': '<f4', 'fortran_order': False, 'shape': (1, 3), }", data),
          "longer than its shape says");
    check("noshape.npy", npyFile(1, "{'descr': '<f4', 'fortran_order': False}", data),
          "has no 'shape'");
    check("badshape.npy",
          npyFile(1, "{'descr': '<f4', 'fortran_order': False, 'shape': (2, x), }", data),
          "malformed shape");
    CHECK_THROWS_WITH(readNpyFloat32(dir.file("absent.npy")), ContainsSubstring("cannot open"));
}

TEST_CASE("readNpyInt32() reads the tracker's reference traces", "[io]") {
    takt4::test::TempDir dir;
    const auto intBytes = [](std::initializer_list<std::int32_t> values) {
        std::string out;
        for (const std::int32_t v : values) {
            char bytes[sizeof v];
            std::memcpy(bytes, &v, sizeof v);
            out.append(bytes, sizeof v);
        }
        return out;
    };
    const std::string data = intBytes({1449, -1, 0, 2147483647});
    spit(dir.file("i4.npy"),
         npyFile(1, "{'descr': '<i4', 'fortran_order': False, 'shape': (2, 2), }", data));
    const takt4::io::NpyInt32Matrix m = takt4::io::readNpyInt32(dir.file("i4.npy"));
    CHECK(m.rows == 2);
    CHECK(m.cols == 2);
    CHECK(m.at(0, 0) == 1449);
    CHECK(m.at(0, 1) == -1);
    CHECK(m.at(1, 1) == 2147483647);

    // The two readers do not accept each other's files; a dtype mix-up would otherwise
    // reinterpret every value silently.
    CHECK_THROWS_WITH(readNpyFloat32(dir.file("i4.npy")), ContainsSubstring("<i4"));
    const std::vector<float> square = {1.0f, 2.0f, 3.0f, 4.0f};
    writeNpyFloat32(dir.file("f4.npy"), 2, 2, square);
    CHECK_THROWS_WITH(takt4::io::readNpyInt32(dir.file("f4.npy")), ContainsSubstring("<f4"));
}

TEST_CASE("the golden synthetic features read as numpy wrote them", "[io]") {
    const std::filesystem::path path =
        std::filesystem::path(TAKT4_TEST_DATA_DIR) / "features" / "synthetic.npy";
    const NpyMatrix m = readNpyFloat32(path);
    CHECK(m.rows == 500); // ten seconds at 50 frames per second
    CHECK(m.cols == 288);
    REQUIRE(m.values.size() == 500 * 288);
    for (std::size_t c = 144; c < 288; ++c) {
        CHECK(m.at(0, c) == 0.0f); // the first frame's differences
    }
    std::size_t negative = 0;
    std::size_t positive = 0;
    for (const float v : m.values) {
        if (v < 0.0f) {
            ++negative;
        }
        if (v > 0.0f) {
            ++positive;
        }
    }
    CHECK(negative == 0);      // log10(1 + x) of a magnitude, and clipped differences
    CHECK(positive == 102395); // numpy: (m > 0).sum()
}
