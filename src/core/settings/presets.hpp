#pragma once

#include "core/settings/settings.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::settings {

/// Show presets: the show half of the settings (`Preset` — the rules, the outputs, the lighting
/// patch, the tempo window and the rest) kept under a name, one file each in a folder beside the
/// settings file. The window saves and loads them, and a show controller loads one by name over
/// OSC (`/ctl/preset`).
///
/// **Never the machine half.** A preset is written without it (`presetJson`), and loading one
/// leaves it alone even when a file dropped in the folder has one: a preset switched from across
/// the room must not change the input, the MIDI controls or the OSC port the switch came in on,
/// and one copied from another rig must not bring that desk's.

/// The longest name a preset may have, in bytes of UTF-8: long enough for "Friday late set —
/// lasers on", short enough to show whole in a list and a reply.
inline constexpr std::size_t kPresetNameMax = 64;

/// `presets` in `settingsDirectory()`, or empty where settings cannot be kept.
std::filesystem::path presetsDirectory();

/// `name` without the spaces and tabs around it: what a typed name is taken as.
std::string trimmedPresetName(std::string_view name);

/// Why `name` cannot be a preset's name, or empty when it can. It is a file's name on every
/// system takt4 runs on, so: not empty, at most `kPresetNameMax` bytes of well-formed UTF-8, none
/// of `< > : " / \ | ? *` or a control character, not starting with a dot or ending with a dot
/// or a space, and not one of the names Windows keeps for devices (CON, PRN, AUX, NUL, COM1 to
/// COM9, LPT1 to LPT9).
std::string presetNameProblem(std::string_view name);

/// The presets in `folder`: the name of every `.json` file in it, less the extension, sorted
/// ignoring the case of A to Z. Names that could not have been saved (`presetNameProblem`) are
/// left out, and so is everything that is not a file. Empty for a folder that is not there.
std::vector<std::string> listPresets(const std::filesystem::path& folder);

/// The one of `names` that `name` asks for: the one spelled the same, or else the one spelled the
/// same ignoring the case of A to Z, when only one is. Nothing otherwise.
std::optional<std::string> findPreset(const std::vector<std::string>& names, std::string_view name);

/// The file preset `name` is kept in, in `folder`.
std::filesystem::path presetFile(const std::filesystem::path& folder, std::string_view name);

/// Writes `preset` as `name` in `folder`, creating the folder, atomically (`saveText`). False with
/// `problem` saying why: a name that cannot be one, or a folder that cannot be written.
bool savePreset(const Preset& preset, const std::filesystem::path& folder, std::string_view name,
                std::string& problem);

/// Deletes preset `name` from `folder`. False with `problem` saying why.
bool deletePreset(const std::filesystem::path& folder, std::string_view name, std::string& problem);

} // namespace takt4::settings
