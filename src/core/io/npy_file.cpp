#include "core/io/npy_file.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

// The samples are copied straight between the file and float storage, which is only
// right on a little-endian host — the only kind takt4 is built for.
static_assert(std::endian::native == std::endian::little, "npy_file assumes a little-endian host");

namespace takt4::io {

namespace {

constexpr std::string_view kMagic = "\x93NUMPY";
constexpr std::size_t kAlignment = 64; // numpy pads the header so the data starts 64-aligned

std::size_t skipSpaces(std::string_view text, std::size_t pos) noexcept {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n')) {
        ++pos;
    }
    return pos;
}

// Position of the value following `'key':` in the header dict, whichever quotes it uses.
std::size_t findValue(std::string_view header, std::string_view key, const std::string& name) {
    for (const char quote : {'\'', '"'}) {
        const std::string quoted = quote + std::string(key) + quote;
        const std::size_t at = header.find(quoted);
        if (at == std::string_view::npos) {
            continue;
        }
        std::size_t pos = skipSpaces(header, at + quoted.size());
        if (pos >= header.size() || header[pos] != ':') {
            break;
        }
        return skipSpaces(header, pos + 1);
    }
    throw std::runtime_error(name + ": .npy header has no '" + std::string(key) + "'");
}

std::string quotedString(std::string_view header, std::size_t pos, const std::string& name) {
    if (pos >= header.size() || (header[pos] != '\'' && header[pos] != '"')) {
        throw std::runtime_error(name + ": .npy header: expected a quoted string");
    }
    const std::size_t end = header.find(header[pos], pos + 1);
    if (end == std::string_view::npos) {
        throw std::runtime_error(name + ": .npy header: unterminated string");
    }
    return std::string(header.substr(pos + 1, end - pos - 1));
}

bool boolean(std::string_view header, std::size_t pos, const std::string& name) {
    if (header.substr(pos, 4) == "True") {
        return true;
    }
    if (header.substr(pos, 5) == "False") {
        return false;
    }
    throw std::runtime_error(name + ": .npy header: expected True or False");
}

std::vector<std::size_t> shapeTuple(std::string_view header, std::size_t pos,
                                    const std::string& name) {
    if (pos >= header.size() || header[pos] != '(') {
        throw std::runtime_error(name + ": .npy header: expected a shape tuple");
    }
    std::vector<std::size_t> shape;
    pos = skipSpaces(header, pos + 1);
    while (pos < header.size() && header[pos] != ')') {
        std::size_t digits = 0;
        unsigned long long value = 0;
        while (pos < header.size() && header[pos] >= '0' && header[pos] <= '9') {
            const unsigned digit = static_cast<unsigned>(header[pos] - '0');
            if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) {
                throw std::runtime_error(name + ": .npy header: shape entry is too large");
            }
            value = value * 10 + digit;
            ++digits;
            ++pos;
        }
        if (digits == 0) {
            throw std::runtime_error(name + ": .npy header: malformed shape");
        }
        shape.push_back(static_cast<std::size_t>(value));
        pos = skipSpaces(header, pos);
        if (pos < header.size() && header[pos] == ',') {
            pos = skipSpaces(header, pos + 1);
        } else if (pos >= header.size() || header[pos] != ')') {
            throw std::runtime_error(name + ": .npy header: malformed shape");
        }
    }
    if (pos >= header.size()) {
        throw std::runtime_error(name + ": .npy header: unterminated shape");
    }
    return shape;
}

/// Opens the file, reads the header, and leaves the stream on the first data byte.
/// `wanted` is the numpy dtype string this reader accepts, e.g. "<f4".
std::vector<std::size_t> openAndParse(const std::filesystem::path& path, std::string_view wanted,
                                      std::ifstream& in, const std::string& name) {
    in.open(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(name + ": cannot open");
    }

    char preamble[8];
    if (!in.read(preamble, sizeof preamble) || std::string_view(preamble, 6) != kMagic) {
        throw std::runtime_error(name + ": not a .npy file");
    }
    const auto major = static_cast<unsigned char>(preamble[6]);
    std::size_t headerLength = 0;
    if (major == 1) {
        unsigned char len[2];
        if (!in.read(reinterpret_cast<char*>(len), 2)) {
            throw std::runtime_error(name + ": truncated .npy header");
        }
        headerLength = static_cast<std::size_t>(len[0] | (len[1] << 8));
    } else if (major == 2 || major == 3) {
        unsigned char len[4];
        if (!in.read(reinterpret_cast<char*>(len), 4)) {
            throw std::runtime_error(name + ": truncated .npy header");
        }
        headerLength = static_cast<std::size_t>(len[0]) | (static_cast<std::size_t>(len[1]) << 8) |
                       (static_cast<std::size_t>(len[2]) << 16) |
                       (static_cast<std::size_t>(len[3]) << 24);
    } else {
        throw std::runtime_error(name + ": .npy format version " + std::to_string(major) +
                                 " is not supported");
    }
    std::string header(headerLength, '\0');
    if (!in.read(header.data(), static_cast<std::streamsize>(headerLength))) {
        throw std::runtime_error(name + ": truncated .npy header");
    }

    const std::string descr = quotedString(header, findValue(header, "descr", name), name);
    if (descr != wanted) {
        throw std::runtime_error(name + ": .npy dtype " + descr + " is not supported here; only '" +
                                 std::string(wanted) + "' is");
    }
    if (boolean(header, findValue(header, "fortran_order", name), name)) {
        throw std::runtime_error(name + ": .npy arrays in Fortran order are not supported");
    }
    const std::vector<std::size_t> shape =
        shapeTuple(header, findValue(header, "shape", name), name);
    if (shape.size() != 2) {
        throw std::runtime_error(name + ": expected a 2-D .npy array, got " +
                                 std::to_string(shape.size()) + " dimensions");
    }
    return shape;
}

template <typename T>
NpyArray<T> readNpy(const std::filesystem::path& path, std::string_view wanted) {
    const std::string name = path.string();
    std::ifstream in;
    const std::vector<std::size_t> shape = openAndParse(path, wanted, in, name);

    NpyArray<T> out;
    out.rows = shape[0];
    out.cols = shape[1];
    if (out.cols != 0 &&
        out.rows > std::numeric_limits<std::size_t>::max() / out.cols / sizeof(T)) {
        throw std::runtime_error(name + ": .npy shape is too large");
    }
    const std::size_t count = out.rows * out.cols;
    out.values.resize(count);
    if (count > 0) {
        const auto bytes = static_cast<std::streamsize>(count * sizeof(T));
        if (!in.read(reinterpret_cast<char*>(out.values.data()), bytes) || in.gcount() != bytes) {
            throw std::runtime_error(name + ": .npy data is shorter than its shape says");
        }
    }
    if (in.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error(name + ": .npy data is longer than its shape says");
    }
    return out;
}

} // namespace

NpyMatrix readNpyFloat32(const std::filesystem::path& path) {
    return readNpy<float>(path, "<f4");
}

NpyInt32Matrix readNpyInt32(const std::filesystem::path& path) {
    return readNpy<std::int32_t>(path, "<i4");
}

void writeNpyFloat32(const std::filesystem::path& path, std::size_t rows, std::size_t cols,
                     std::span<const float> values) {
    if (values.size() != rows * cols) {
        throw std::invalid_argument("writeNpyFloat32: " + std::to_string(values.size()) +
                                    " values do not fill " + std::to_string(rows) + " x " +
                                    std::to_string(cols));
    }
    // The header numpy itself writes, padding included: the dict, spaces up to a
    // 64-byte boundary for the whole preamble, and a newline.
    std::string header = "{'descr': '<f4', 'fortran_order': False, 'shape': (" +
                         std::to_string(rows) + ", " + std::to_string(cols) + "), }";
    const std::size_t preamble = kMagic.size() + 2 + 2; // magic, version, uint16 length
    const std::size_t padding = kAlignment - (preamble + header.size() + 1) % kAlignment;
    header.append(padding, ' ');
    header.push_back('\n');
    if (header.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("writeNpyFloat32: header does not fit format version 1.0");
    }

    const std::string name = path.string();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error(name + ": cannot create");
    }
    const char version[2] = {1, 0};
    const char length[2] = {static_cast<char>(header.size() & 0xFF),
                            static_cast<char>(header.size() >> 8)};
    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    out.write(version, 2);
    out.write(length, 2);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!values.empty()) {
        out.write(reinterpret_cast<const char*>(values.data()),
                  static_cast<std::streamsize>(values.size() * sizeof(float)));
    }
    out.flush();
    if (!out) {
        throw std::runtime_error(name + ": write failed");
    }
}

} // namespace takt4::io
