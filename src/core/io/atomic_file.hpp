#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace takt4::io {

/// Replaces the file at `path` with `bytes` **so that nothing that happens part-way through
/// can leave half of it**.
///
/// The bytes go to `<path>.tmp` beside it first, are flushed to the disk, and only then is the
/// temporary renamed over the real name — a rename the file system does in one step. A crash,
/// a power cut or a full disk mid-write therefore leaves the old file exactly as it was, and
/// a reader never sees anything but the whole old file or the whole new one.
///
/// Written for `settings.json`, which is the whole show: the outputs, the rules, the patch and
/// the MIDI bindings. It used to be truncated and rewritten in place, so one interrupted save
/// was an empty file and a rig somebody had built that afternoon was gone.
///
/// False when it could not be done — a directory that is not there or not writable, a disk
/// that filled, a destination another program holds open — and then the old file, if there
/// was one, is untouched and the temporary is removed. Never throws.
bool replaceFile(const std::filesystem::path& path, std::string_view bytes) noexcept;

/// The whole of a file, or nothing with `problem` saying why in a few words ("it is a folder,
/// not a file", "permission denied"). Distinguishes a file that is not there — `problem` left
/// empty — from one that is there and could not be read, which is the difference between a
/// fresh install and a file somebody must not have saved over.
std::optional<std::string> readFile(const std::filesystem::path& path, std::string& problem);

} // namespace takt4::io
