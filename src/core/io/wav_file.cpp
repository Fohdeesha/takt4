#include "core/io/wav_file.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <ios>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace takt4::io {

namespace {

// WAV is little-endian throughout; the fields are assembled byte by byte so that this
// reads the same on any host.
std::uint16_t readU16(const unsigned char* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t readU32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool tagIs(const unsigned char* p, const char (&tag)[5]) noexcept {
    return std::memcmp(p, tag, 4) == 0;
}

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatIeeeFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

// Everything but the leading format tag of KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT.
constexpr std::array<unsigned char, 14> kSubformatGuidTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                              0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

struct Format {
    std::uint16_t tag = 0; // kFormatPcm or kFormatIeeeFloat, the extensible wrapper resolved
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
};

Format parseFormat(const unsigned char* chunk, std::size_t size, const std::string& name) {
    if (size < 16) {
        throw std::runtime_error(name + ": fmt chunk is too short (" + std::to_string(size) + " bytes)");
    }
    Format f;
    f.tag = readU16(chunk);
    f.channels = readU16(chunk + 2);
    f.sampleRate = readU32(chunk + 4);
    f.blockAlign = readU16(chunk + 12);
    f.bitsPerSample = readU16(chunk + 14);

    if (f.tag == kFormatExtensible) {
        // cbSize, wValidBitsPerSample, dwChannelMask, SubFormat GUID.
        if (size < 40 || readU16(chunk + 16) < 22) {
            throw std::runtime_error(name + ": WAVE_FORMAT_EXTENSIBLE header is too short");
        }
        const std::uint16_t validBits = readU16(chunk + 18);
        if (validBits != 0 && validBits != f.bitsPerSample) {
            throw std::runtime_error(name + ": " + std::to_string(validBits) + " valid bits in " +
                                     std::to_string(f.bitsPerSample) + "-bit containers is not supported");
        }
        if (std::memcmp(chunk + 26, kSubformatGuidTail.data(), kSubformatGuidTail.size()) != 0) {
            throw std::runtime_error(name + ": unknown WAVE_FORMAT_EXTENSIBLE subformat");
        }
        f.tag = readU16(chunk + 24);
    }

    if (f.tag == kFormatPcm) {
        if (f.bitsPerSample != 16 && f.bitsPerSample != 24 && f.bitsPerSample != 32) {
            throw std::runtime_error(name + ": " + std::to_string(f.bitsPerSample) +
                                     "-bit PCM is not supported (16, 24 and 32 are)");
        }
    } else if (f.tag == kFormatIeeeFloat) {
        if (f.bitsPerSample != 32) {
            throw std::runtime_error(name + ": " + std::to_string(f.bitsPerSample) +
                                     "-bit float is not supported (32 is)");
        }
    } else {
        throw std::runtime_error(name + ": format tag " + std::to_string(f.tag) +
                                 " is not supported (PCM and IEEE float are)");
    }
    if (f.channels == 0) {
        throw std::runtime_error(name + ": no channels");
    }
    if (f.sampleRate == 0) {
        throw std::runtime_error(name + ": sample rate is zero");
    }
    if (f.blockAlign != f.channels * (f.bitsPerSample / 8)) {
        throw std::runtime_error(name + ": block align " + std::to_string(f.blockAlign) + " does not match " +
                                 std::to_string(f.channels) + " channels of " + std::to_string(f.bitsPerSample) +
                                 " bits");
    }
    return f;
}

void decode(const Format& f, const unsigned char* data, std::size_t size, std::vector<float>& out) {
    const std::size_t bytesPerSample = f.bitsPerSample / 8u;
    const std::size_t count = size / bytesPerSample;
    out.resize(count);
    if (f.tag == kFormatIeeeFloat) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t bits = readU32(data + 4 * i);
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof value);
            out[i] = value;
        }
        return;
    }
    // Integer PCM: q / 2^(bits − 1). The conversion to float rounds only for 32-bit
    // words wider than a float's mantissa; the scaling by a power of two is exact.
    switch (f.bitsPerSample) {
    case 16:
        for (std::size_t i = 0; i < count; ++i) {
            const auto q = static_cast<std::int16_t>(readU16(data + 2 * i));
            out[i] = static_cast<float>(q) * (1.0f / 32768.0f);
        }
        break;
    case 24:
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned char* p = data + 3 * i;
            // The 24 bits go to the top of a 32-bit word so that the shift sign-extends.
            const std::uint32_t bits = (static_cast<std::uint32_t>(p[0]) << 8) |
                                       (static_cast<std::uint32_t>(p[1]) << 16) |
                                       (static_cast<std::uint32_t>(p[2]) << 24);
            const std::int32_t q = static_cast<std::int32_t>(bits) >> 8;
            out[i] = static_cast<float>(q) * (1.0f / 8388608.0f);
        }
        break;
    case 32:
        for (std::size_t i = 0; i < count; ++i) {
            const auto q = static_cast<std::int32_t>(readU32(data + 4 * i));
            out[i] = static_cast<float>(q) * (1.0f / 2147483648.0f);
        }
        break;
    default:
        throw std::logic_error("wav_file: parseFormat admitted an unsupported sample size");
    }
}

std::vector<unsigned char> readWholeFile(const std::filesystem::path& path, const std::string& name) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(name + ": cannot open");
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error(name + ": cannot determine the file size");
    }
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!in || in.gcount() != size) {
        throw std::runtime_error(name + ": read failed");
    }
    return bytes;
}

} // namespace

WavData readWavFile(const std::filesystem::path& path) {
    const std::string name = path.string();
    const std::vector<unsigned char> file = readWholeFile(path, name);

    if (file.size() < 12 || !tagIs(file.data(), "RIFF") || !tagIs(file.data() + 8, "WAVE")) {
        throw std::runtime_error(name + ": not a RIFF/WAVE file");
    }
    // The RIFF size field is ignored: it is wrong in enough files to be worthless.

    std::optional<Format> format;
    std::optional<std::pair<std::size_t, std::size_t>> data; // offset, size
    std::size_t offset = 12;
    while (offset + 8 <= file.size()) {
        const unsigned char* header = file.data() + offset;
        const std::size_t size = readU32(header + 4);
        const std::size_t body = offset + 8;
        if (size > file.size() - body) {
            throw std::runtime_error(name + ": chunk '" + std::string(reinterpret_cast<const char*>(header), 4) +
                                     "' runs past the end of the file");
        }
        if (tagIs(header, "fmt ")) {
            if (format) {
                throw std::runtime_error(name + ": more than one fmt chunk");
            }
            format = parseFormat(file.data() + body, size, name);
        } else if (tagIs(header, "data")) {
            if (data) {
                throw std::runtime_error(name + ": more than one data chunk");
            }
            data = std::make_pair(body, size);
        }
        offset = body + size + (size % 2); // chunks are word-aligned; the pad byte is not counted
    }
    if (!format) {
        throw std::runtime_error(name + ": no fmt chunk");
    }
    if (!data) {
        throw std::runtime_error(name + ": no data chunk");
    }
    if (data->second % format->blockAlign != 0) {
        throw std::runtime_error(name + ": data chunk of " + std::to_string(data->second) +
                                 " bytes is not a whole number of frames");
    }

    WavData out;
    out.sampleRate = format->sampleRate;
    out.channels = format->channels;
    decode(*format, file.data() + data->first, data->second, out.samples);
    return out;
}

} // namespace takt4::io
