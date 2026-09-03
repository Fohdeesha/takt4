#include "core/io/wav_file.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using takt4::io::readWavFile;
using takt4::io::WavData;

namespace {

// Builds WAV files byte by byte, so the reader is tested against the format and not
// against some other writer's idea of it.
class WavBuilder {
public:
    WavBuilder& tag(std::string_view four) {
        for (const char c : four) {
            bytes_.push_back(static_cast<unsigned char>(c));
        }
        return *this;
    }
    WavBuilder& u16(std::uint32_t v) {
        bytes_.push_back(static_cast<unsigned char>(v & 0xFF));
        bytes_.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        return *this;
    }
    WavBuilder& u32(std::uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
        return *this;
    }
    WavBuilder& raw(std::initializer_list<unsigned char> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        return *this;
    }

    // "RIFF" <size> "WAVE"; the size is filled in by write().
    WavBuilder& riff() {
        return tag("RIFF").u32(0).tag("WAVE");
    }
    // A 16-byte fmt chunk.
    WavBuilder& fmt(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits) {
        const std::uint32_t blockAlign = channels * (bits / 8u);
        return tag("fmt ").u32(16).u16(format).u16(channels).u32(rate).u32(rate * blockAlign).u16(blockAlign).u16(bits);
    }
    // A 40-byte WAVE_FORMAT_EXTENSIBLE fmt chunk wrapping `subformat` (1 PCM, 3 float).
    WavBuilder& fmtExtensible(std::uint16_t subformat, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits) {
        const std::uint32_t blockAlign = channels * (bits / 8u);
        tag("fmt ").u32(40).u16(0xFFFE).u16(channels).u32(rate).u32(rate * blockAlign).u16(blockAlign).u16(bits);
        u16(22).u16(bits).u32(0x3); // cbSize, valid bits, channel mask (front left + right)
        u16(subformat);
        return raw({0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});
    }
    WavBuilder& data16(std::initializer_list<std::int16_t> samples) {
        tag("data").u32(static_cast<std::uint32_t>(samples.size() * 2));
        for (const std::int16_t s : samples) {
            u16(static_cast<std::uint16_t>(s));
        }
        return *this;
    }

    std::filesystem::path write(const std::filesystem::path& path) {
        if (bytes_.size() >= 8) { // patch the RIFF size: everything after it
            const std::uint32_t size = static_cast<std::uint32_t>(bytes_.size() - 8);
            bytes_[4] = static_cast<unsigned char>(size & 0xFF);
            bytes_[5] = static_cast<unsigned char>((size >> 8) & 0xFF);
            bytes_[6] = static_cast<unsigned char>((size >> 16) & 0xFF);
            bytes_[7] = static_cast<unsigned char>(size >> 24);
        }
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
        REQUIRE(out.good());
        return path;
    }

    std::vector<unsigned char>& bytes() { return bytes_; }

private:
    std::vector<unsigned char> bytes_;
};

} // namespace

TEST_CASE("readWavFile() decodes 16-bit PCM the way libsndfile does", "[io]") {
    takt4::test::TempDir dir;
    WavBuilder wav;
    wav.riff().fmt(1, 2, 44100, 16).data16({-32768, 32767, -1, 1, 0, 16384});
    const WavData got = readWavFile(wav.write(dir.file("pcm16.wav")));

    CHECK(got.sampleRate == 44100);
    CHECK(got.channels == 2);
    CHECK(got.frames() == 3);
    REQUIRE(got.samples.size() == 6);
    CHECK(got.samples[0] == -1.0f);
    CHECK(got.samples[1] == 32767.0f / 32768.0f);
    CHECK(got.samples[2] == -1.0f / 32768.0f);
    CHECK(got.samples[3] == 1.0f / 32768.0f);
    CHECK(got.samples[4] == 0.0f);
    CHECK(got.samples[5] == 0.5f);
}

TEST_CASE("readWavFile() decodes 24-bit, 32-bit and float PCM", "[io]") {
    takt4::test::TempDir dir;

    SECTION("24-bit: three little-endian bytes, sign in the top one") {
        WavBuilder wav;
        wav.riff().fmt(1, 1, 22050, 24).tag("data").u32(12);
        wav.raw({0x00, 0x00, 0x80}); // -8388608
        wav.raw({0xFF, 0xFF, 0x7F}); // 8388607
        wav.raw({0x01, 0x00, 0x00}); // 1
        wav.raw({0x00, 0x00, 0xC0}); // -4194304
        const WavData got = readWavFile(wav.write(dir.file("pcm24.wav")));
        REQUIRE(got.samples.size() == 4);
        CHECK(got.samples[0] == -1.0f);
        CHECK(got.samples[1] == 8388607.0f / 8388608.0f);
        CHECK(got.samples[2] == 1.0f / 8388608.0f);
        CHECK(got.samples[3] == -0.5f);
    }

    SECTION("32-bit integer") {
        WavBuilder wav;
        wav.riff().fmt(1, 1, 48000, 32).tag("data").u32(12).u32(0x80000000u).u32(0x7FFFFFFFu).u32(0x40000000u);
        const WavData got = readWavFile(wav.write(dir.file("pcm32.wav")));
        REQUIRE(got.samples.size() == 3);
        CHECK(got.samples[0] == -1.0f);
        CHECK(got.samples[1] == 1.0f); // 2^31 − 1 rounds up to 2^31 in float, as libsndfile's does
        CHECK(got.samples[2] == 0.5f);
    }

    SECTION("32-bit float, taken as is") {
        WavBuilder wav;
        wav.riff().fmt(3, 1, 22050, 32).tag("data").u32(12);
        for (const float v : {0.75f, -1.5f, 1e-20f}) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof bits);
            wav.u32(bits);
        }
        const WavData got = readWavFile(wav.write(dir.file("float32.wav")));
        REQUIRE(got.samples.size() == 3);
        CHECK(got.samples[0] == 0.75f);
        CHECK(got.samples[1] == -1.5f);
        CHECK(got.samples[2] == 1e-20f);
    }
}

TEST_CASE("readWavFile() handles WAVE_FORMAT_EXTENSIBLE and foreign chunks", "[io]") {
    takt4::test::TempDir dir;

    SECTION("extensible PCM, with a LIST chunk of odd size before the data and junk after it") {
        WavBuilder wav;
        wav.riff().fmtExtensible(1, 2, 96000, 16);
        wav.tag("LIST").u32(5).raw({'I', 'N', 'F', 'O', 'x'}).raw({0}); // 5 bytes plus the pad byte
        wav.data16({100, -100, 200, -200});
        wav.tag("junk").u32(4).raw({1, 2, 3, 4});
        const WavData got = readWavFile(wav.write(dir.file("extensible.wav")));
        CHECK(got.sampleRate == 96000);
        CHECK(got.channels == 2);
        REQUIRE(got.samples.size() == 4);
        CHECK(got.samples[0] == 100.0f / 32768.0f);
        CHECK(got.samples[3] == -200.0f / 32768.0f);
    }

    SECTION("extensible float") {
        WavBuilder wav;
        wav.riff().fmtExtensible(3, 1, 22050, 32).tag("data").u32(4).u32(0x3F800000u); // 1.0f
        const WavData got = readWavFile(wav.write(dir.file("extensible-float.wav")));
        REQUIRE(got.samples.size() == 1);
        CHECK(got.samples[0] == 1.0f);
    }
}

TEST_CASE("readWavFile() refuses what it cannot read, and says why", "[io]") {
    takt4::test::TempDir dir;

    SECTION("not a WAV") {
        std::ofstream(dir.file("text.wav")) << "hello";
        CHECK_THROWS_WITH(readWavFile(dir.file("text.wav")), ContainsSubstring("not a RIFF/WAVE file"));
    }
    SECTION("missing file") {
        CHECK_THROWS_WITH(readWavFile(dir.file("absent.wav")), ContainsSubstring("cannot open"));
    }
    SECTION("compressed") {
        WavBuilder wav;
        wav.riff().fmt(2, 1, 8000, 4).data16({0}); // ADPCM
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("adpcm.wav"))), ContainsSubstring("format tag 2"));
    }
    SECTION("8-bit PCM") {
        WavBuilder wav;
        wav.riff().fmt(1, 1, 8000, 8).tag("data").u32(2).raw({0x80, 0x80});
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("pcm8.wav"))), ContainsSubstring("8-bit PCM"));
    }
    SECTION("64-bit float") {
        WavBuilder wav;
        wav.riff().fmt(3, 1, 8000, 64).tag("data").u32(0);
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("float64.wav"))), ContainsSubstring("64-bit float"));
    }
    SECTION("no data chunk") {
        WavBuilder wav;
        wav.riff().fmt(1, 1, 8000, 16);
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("nodata.wav"))), ContainsSubstring("no data chunk"));
    }
    SECTION("no fmt chunk") {
        WavBuilder wav;
        wav.riff().data16({0, 0});
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("nofmt.wav"))), ContainsSubstring("no fmt chunk"));
    }
    SECTION("data chunk longer than the file") {
        WavBuilder wav;
        wav.riff().fmt(1, 1, 8000, 16).tag("data").u32(1000).u16(0);
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("truncated.wav"))),
                          ContainsSubstring("runs past the end of the file"));
    }
    SECTION("data chunk that is not whole frames") {
        WavBuilder wav;
        wav.riff().fmt(1, 2, 8000, 16).tag("data").u32(6).u16(0).u16(0).u16(0);
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("ragged.wav"))),
                          ContainsSubstring("not a whole number of frames"));
    }
    SECTION("unknown extensible subformat") {
        WavBuilder wav;
        wav.riff().fmtExtensible(1, 1, 8000, 16);
        wav.bytes()[wav.bytes().size() - 1] ^= 0xFF; // corrupt the GUID's last byte
        wav.data16({0});
        CHECK_THROWS_WITH(readWavFile(wav.write(dir.file("subformat.wav"))),
                          ContainsSubstring("unknown WAVE_FORMAT_EXTENSIBLE subformat"));
    }
}

TEST_CASE("the golden synthetic excerpt reads as tools/make_golden.py wrote it", "[io]") {
    const std::filesystem::path path = std::filesystem::path(TAKT4_TEST_DATA_DIR) / "features" / "synthetic.wav";
    const WavData got = readWavFile(path);
    CHECK(got.sampleRate == 22050);
    CHECK(got.channels == 1);
    CHECK(got.frames() == 220500); // ten seconds
    float peak = 0.0f;
    for (const float s : got.samples) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak == 26214.0f / 32768.0f); // the sidecar's "peak": 26214
}
