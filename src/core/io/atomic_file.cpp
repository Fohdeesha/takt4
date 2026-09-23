#include "core/io/atomic_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace takt4::io {

bool replaceFile(const std::filesystem::path& path, std::string_view bytes) noexcept {
    try {
        if (path.empty()) {
            return false;
        }
        std::filesystem::path temp = path;
        temp += ".tmp";
#if defined(_WIN32)
        // Not shared while it is written: a second takt4 saving at the same instant is refused
        // its own open rather than interleaving bytes with this one, and the rename below fails
        // on a file another process still holds, rather than moving a half-written one.
        const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        bool ok = true;
        std::size_t done = 0;
        while (ok && done < bytes.size()) {
            const auto chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30));
            DWORD written = 0;
            ok = WriteFile(file, bytes.data() + done, chunk, &written, nullptr) != 0 &&
                 written == chunk;
            done += written;
        }
        // To the disk, not only to the cache: the rename is durable on its own
        // (MOVEFILE_WRITE_THROUGH), and a rename that reached the disk ahead of the data it
        // names would be a whole file of zeros after a power cut.
        ok = ok && FlushFileBuffers(file) != 0;
        // A close can fail too — a network share reports a write it could not complete here —
        // and a failed close is a failed save.
        ok = (CloseHandle(file) != 0) && ok;
        if (ok) {
            ok = MoveFileExW(temp.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        }
        if (!ok) {
            DeleteFileW(temp.c_str());
        }
        return ok;
#else
        const int file = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (file < 0) {
            return false;
        }
        bool ok = true;
        std::size_t done = 0;
        while (ok && done < bytes.size()) {
            const ::ssize_t written = ::write(file, bytes.data() + done, bytes.size() - done);
            if (written < 0) {
                ok = errno == EINTR;
                continue;
            }
            done += static_cast<std::size_t>(written);
        }
        ok = ok && ::fsync(file) == 0;
        ok = (::close(file) == 0) && ok;
        if (ok) {
            ok = ::rename(temp.c_str(), path.c_str()) == 0;
        }
        if (!ok) {
            ::unlink(temp.c_str());
        }
        return ok;
#endif
    } catch (...) {
        // Only the path arithmetic can throw, and only by failing to allocate. Nothing has been
        // touched by then.
        return false;
    }
}

std::optional<std::string> readFile(const std::filesystem::path& path, std::string& problem) {
    problem.clear();
    std::error_code code;
    const std::filesystem::file_status status = std::filesystem::status(path, code);
    if (code && status.type() != std::filesystem::file_type::not_found) {
        // The file system would not even say whether it is there — a folder that cannot be
        // listed, a share that has gone away. That is a file that could not be read, not a
        // fresh install, and treating it as one would let a save go over it.
        problem = code.message();
        return std::nullopt;
    }
    if (!std::filesystem::exists(status)) {
        return std::nullopt; // not there, which is not a problem
    }
    if (std::filesystem::is_directory(status)) {
        problem = "it is a folder, not a file";
        return std::nullopt;
    }
    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        const int error = errno;
        problem = error != 0 ? std::generic_category().message(error)
                             : std::string("it could not be opened");
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        problem = "it could not be read to the end";
        return std::nullopt;
    }
    return text.str();
}

} // namespace takt4::io
