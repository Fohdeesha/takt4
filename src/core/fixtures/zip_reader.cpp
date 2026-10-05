#include "core/fixtures/zip_reader.hpp"

#include <miniz.h>

#include <fstream>
#include <string>
#include <system_error>

namespace takt4::fixtures {
namespace {

/// Where an inflated member goes, a piece at a time, and where it stops.
struct Sink {
    std::string bytes;
    std::size_t limit = 0;
    bool overflowed = false;
};

std::size_t collect(void* opaque, mz_uint64 /*offset*/, const void* data, std::size_t size) {
    auto& sink = *static_cast<Sink*>(opaque);
    if (size > sink.limit - sink.bytes.size()) {
        // Short of what was offered is how a callback stops miniz: the extraction fails, and
        // nothing past the limit is ever held.
        sink.overflowed = true;
        return 0;
    }
    sink.bytes.append(static_cast<const char*>(data), size);
    return size;
}

/// miniz's own words for an error.
std::string wordsOf(mz_zip_error error) {
    const char* words = mz_zip_get_error_string(error);
    return words != nullptr ? words : "unknown error";
}

std::string describe(mz_zip_error error) {
    switch (error) {
    case MZ_ZIP_NOT_AN_ARCHIVE:
    case MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE:
    case MZ_ZIP_INVALID_HEADER_OR_CORRUPTED:
    case MZ_ZIP_DECOMPRESSION_FAILED:
    case MZ_ZIP_CRC_CHECK_FAILED:
    case MZ_ZIP_UNSUPPORTED_CDIR_SIZE:
    case MZ_ZIP_UNSUPPORTED_MULTIDISK:
        break;
    case MZ_ZIP_FILE_READ_FAILED:
    case MZ_ZIP_FILE_SEEK_FAILED:
    case MZ_ZIP_FILE_STAT_FAILED:
    case MZ_ZIP_FILE_OPEN_FAILED:
        return "it could not be read";
    case MZ_ZIP_ALLOC_FAILED:
        return "there was not enough memory to read it";
    case MZ_ZIP_UNSUPPORTED_ENCRYPTION:
        return "it is password-protected";
    case MZ_ZIP_UNSUPPORTED_METHOD:
        return "it is packed in a way takt4 can't unpack";
    default:
        break;
    }
    // miniz's own words for the rest ("not an archive", "invalid header or archive is
    // corrupted") after the plain ones: a GDTF file is the only archive takt4 opens, and one
    // that will not open is damaged or was never one.
    return "it is damaged, or not a GDTF file (" + wordsOf(error) + ")";
}

/// Why one member of an archive that opened could not be unpacked: the file is damaged — said of
/// the member, which is the part a person opening the file can go and look at.
std::string describeMember(const std::string& inside, mz_zip_error error) {
    switch (error) {
    case MZ_ZIP_FILE_READ_FAILED:
    case MZ_ZIP_FILE_SEEK_FAILED:
    case MZ_ZIP_FILE_STAT_FAILED:
    case MZ_ZIP_FILE_OPEN_FAILED:
    case MZ_ZIP_ALLOC_FAILED:
    case MZ_ZIP_UNSUPPORTED_ENCRYPTION:
    case MZ_ZIP_UNSUPPORTED_METHOD:
        return describe(error);
    default:
        return inside + " can't be unpacked, so the file is damaged (" + wordsOf(error) + ")";
    }
}

/// The member from an archive miniz has opened. Ends the archive whatever happens.
std::optional<std::string> extract(mz_zip_archive& zip, std::string_view name, std::size_t limit,
                                   std::string& problem) {
    const std::string quoted(name);
    const int index =
        mz_zip_reader_locate_file(&zip, quoted.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    // Said of the file the operator picked, with the member named in quotes for whoever opens it.
    const std::string inside = "\"" + quoted + "\" inside it";
    if (index < 0) {
        problem = "there is no " + inside + ", so it isn't a GDTF file";
        return std::nullopt;
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(index), &stat)) {
        problem = describeMember(inside, mz_zip_get_last_error(&zip));
        return std::nullopt;
    }
    if (stat.m_is_directory) {
        problem = inside + " is a folder, not a file";
        return std::nullopt;
    }
    if (stat.m_is_encrypted) {
        problem = "it is password-protected";
        return std::nullopt;
    }
    if (!stat.m_is_supported || (stat.m_method != 0 && stat.m_method != MZ_DEFLATED)) {
        problem = "it is packed in a way takt4 can't unpack";
        return std::nullopt;
    }
    if (stat.m_uncomp_size > limit) {
        problem = inside + " is " + std::to_string(stat.m_uncomp_size / (1024 * 1024)) +
                  " MB, too big for a fixture file (takt4 reads up to " +
                  std::to_string(limit / (1024 * 1024)) + " MB)";
        return std::nullopt;
    }
    Sink sink;
    sink.limit = limit;
    const bool ok =
        mz_zip_reader_extract_to_callback(&zip, static_cast<mz_uint>(index), collect, &sink, 0);
    if (sink.overflowed) {
        problem = inside + " is bigger than the file says it is, so the file is damaged";
        return std::nullopt;
    }
    if (!ok) {
        problem = describeMember(inside, mz_zip_get_last_error(&zip));
        return std::nullopt;
    }
    return std::move(sink.bytes);
}

/// The archive, read through this handle at whatever offset miniz asks for.
struct FileSource {
    std::ifstream stream;
};

std::size_t readAt(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
    auto& source = *static_cast<FileSource*>(opaque);
    source.stream.clear();
    source.stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!source.stream) {
        return 0;
    }
    source.stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(source.stream.gcount());
}

} // namespace

std::optional<std::string> readZipMember(std::span<const std::uint8_t> archive,
                                         std::string_view name, std::size_t limit,
                                         std::string& problem) {
    problem.clear();
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (archive.empty() || !mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) {
        problem =
            archive.empty() ? std::string("it is empty") : describe(mz_zip_get_last_error(&zip));
        mz_zip_reader_end(&zip);
        return std::nullopt;
    }
    std::optional<std::string> member = extract(zip, name, limit, problem);
    mz_zip_reader_end(&zip);
    return member;
}

std::optional<std::string> readZipMemberFromFile(const std::filesystem::path& path,
                                                 std::string_view name, std::size_t limit,
                                                 std::string& problem) {
    problem.clear();
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code) {
        problem = "it could not be read (" + code.message() + ")";
        return std::nullopt;
    }
    if (size == 0) {
        problem = "it is empty";
        return std::nullopt;
    }
    FileSource source;
    source.stream.open(path, std::ios::binary);
    if (!source.stream) {
        problem = "it could not be opened";
        return std::nullopt;
    }
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    zip.m_pRead = readAt;
    zip.m_pIO_opaque = &source;
    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(size), 0)) {
        problem = describe(mz_zip_get_last_error(&zip));
        mz_zip_reader_end(&zip);
        return std::nullopt;
    }
    std::optional<std::string> member = extract(zip, name, limit, problem);
    mz_zip_reader_end(&zip);
    return member;
}

} // namespace takt4::fixtures
