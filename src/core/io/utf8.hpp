#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace takt4::io {

/// Whether `text` is well-formed UTF-8 by the strict definition — no overlong forms, no
/// surrogate halves, nothing past U+10FFFF — which is Rust's `str::from_utf8` and therefore
/// Slint's.
bool isValidUtf8(std::string_view text) noexcept;

/// `text` with every byte that does not begin a well-formed UTF-8 sequence replaced by U+FFFD.
///
/// **Everything handed to the window goes through this first**, and the reason is a crash
/// rather than a rendering nicety. Slint builds a `SharedString` with
/// `core::str::from_utf8(..).unwrap()`, so one ill-formed byte is a Rust panic across the C
/// ABI — an abort, with no message and no chance to catch it (see `trigger::Value`'s
/// `utf8Fit`, which met it first). And a good share of the text takt4 shows was never
/// promised to be UTF-8 at all: an ASIO driver's name and its channel names come out of the
/// registry and the driver through the *ANSI* API, RtMidi names ports as the platform does,
/// and an exception's message is whatever the library that threw it wrote. A driver called
/// "Interface Née" is enough.
///
/// Returns the input unchanged — no allocation beyond the copy — when it is already valid,
/// which is every string in the ordinary case.
std::string validUtf8(std::string_view text);

/// A path as UTF-8 text, for putting in front of a person.
///
/// **Not `path.string()`.** On Windows that converts through the ANSI code page, which does
/// two different bad things depending on the name. A character the code page has — é, an en
/// dash, a curly apostrophe — comes back as a single byte that is not UTF-8, and goes on to
/// abort Slint as `validUtf8` describes. A character it has not got makes `string()` *throw*,
/// and a status line is built inside a Slint callback, where an exception also ends the
/// process. "Jon’s set.json" and a folder called "Shows – 2026" were each enough.
///
/// This converts the path's own UTF-16 on Windows (an unpaired surrogate, which NTFS allows,
/// becomes U+FFFD) and validates the native bytes everywhere else. It never throws.
std::string pathText(const std::filesystem::path& path) noexcept;

} // namespace takt4::io
