#include "core/fixtures/import.hpp"

#include "core/fixtures/gdtf_reader.hpp"
#include "core/fixtures/ofl_reader.hpp"
#include "core/fixtures/profile_mapper.hpp"
#include "core/fixtures/text.hpp"
#include "core/io/utf8.hpp"

#include <exception>
#include <fstream>
#include <new>
#include <system_error>

namespace takt4::fixtures {
namespace {

enum class Format { Gdtf, Ofl, Unknown };

Format formatOf(const std::filesystem::path& path) {
    const std::string extension = text::lowerAscii(io::pathText(path.extension()));
    if (extension == ".gdtf") {
        return Format::Gdtf;
    }
    if (extension == ".json") {
        return Format::Ofl;
    }
    // Neither: look at the first bytes. A zip archive starts "PK\3\4"; an OFL file is a JSON
    // object, after any byte order mark and white space.
    std::ifstream stream(path, std::ios::binary);
    char head[64] = {};
    stream.read(head, sizeof head);
    const std::string_view start(head, static_cast<std::size_t>(stream.gcount()));
    if (start.starts_with(std::string_view("PK\x03\x04", 4))) {
        return Format::Gdtf;
    }
    std::string_view rest = start;
    if (rest.starts_with("\xEF\xBB\xBF")) {
        rest.remove_prefix(3);
    }
    if (text::trim(rest).starts_with('{')) {
        return Format::Ofl;
    }
    return Format::Unknown;
}

} // namespace

ReadResult readDefinitionFile(const std::filesystem::path& path) {
    try {
        std::error_code code;
        if (!std::filesystem::is_regular_file(path, code)) {
            ReadResult result;
            result.problem = code ? "it could not be read (" + code.message() + ")"
                                  : std::string("it is not a file");
            return result;
        }
        switch (formatOf(path)) {
        case Format::Gdtf:
            return readGdtfFile(path);
        case Format::Ofl:
            return readOflFile(path);
        case Format::Unknown:
            break;
        }
        ReadResult result;
        result.problem = "it is neither a GDTF file nor an Open Fixture Library file";
        return result;
    } catch (const std::bad_alloc&) {
        ReadResult result;
        result.problem = "there was not enough memory to read it";
        return result;
    } catch (const std::exception& error) {
        // Nothing in the readers is meant to throw; this is the belt to that strap, since a
        // throw out of here would cross back into the window that asked.
        ReadResult result;
        result.problem = "it could not be read (" + text::clean(error.what(), 200) + ")";
        return result;
    }
}

std::optional<FixtureProfile> importProfile(const std::filesystem::path& path,
                                            std::string& problem) {
    problem.clear();
    ReadResult read = readDefinitionFile(path);
    if (!read.definition) {
        problem = read.problem;
        return std::nullopt;
    }
    return mapDefinition(*read.definition);
}

} // namespace takt4::fixtures
