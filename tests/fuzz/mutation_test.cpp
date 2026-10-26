// Every reader of bytes from outside takt4, fed what it reads mangled in every way a cheap
// mutator can think of: a bit flipped, a byte set to a value that means something, bytes put in
// or taken out, the end cut off, a run copied over another, a length field made huge. The bar
// is the show's — nothing crashes, hangs or reads past what it was given, which the
// AddressSanitizer run is there to see — and whatever does get read is safe to show and to save.
//
// Seeded and repeatable. TAKT4_FUZZ_ROUNDS runs a longer campaign, TAKT4_FUZZ_SEED another one.

#include "core/control/midi_binding.hpp"
#include "core/control/osc_parse.hpp"
#include "core/dmx/fixture.hpp"
#include "core/fixtures/gdtf_reader.hpp"
#include "core/fixtures/ofl_reader.hpp"
#include "core/fixtures/profile_mapper.hpp"
#include "core/io/utf8.hpp"
#include "core/output/osc_message.hpp"
#include "core/output/output_target.hpp"
#include "core/settings/settings.hpp"
#include "core/timing/prolink/packets.hpp"
#include "core/trigger/rule.hpp"

#include "fixtures/fixture_files.hpp"
#include "support/prolink_capture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::uint32_t environmentNumber(const char* name, std::uint32_t otherwise) {
    std::string text;
#if defined(_MSC_VER)
    char buffer[32] = {};
    std::size_t length = 0;
    if (getenv_s(&length, buffer, sizeof buffer, name) == 0 && length > 1) {
        text = buffer;
    }
#else
    if (const char* value = std::getenv(name)) {
        text = value;
    }
#endif
    return text.empty() ? otherwise : static_cast<std::uint32_t>(std::stoul(text));
}

/// How many mangled inputs each reader is given.
std::uint32_t rounds(std::uint32_t otherwise) {
    return environmentNumber("TAKT4_FUZZ_ROUNDS", otherwise);
}

/// Mangles bytes, from a fixed seed: the same mangling on every compiler and every run.
class Mutator {
public:
    explicit Mutator(std::uint32_t salt)
        : state_((environmentNumber("TAKT4_FUZZ_SEED", 0x2545F491U) ^ (salt * 0x9E3779B9U)) | 1U) {}

    std::uint32_t next() {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 17U;
        state_ ^= state_ << 5U;
        return state_;
    }
    std::size_t pick(std::size_t count) { return count == 0 ? 0 : next() % count; }

    /// One to four manglings of `data`.
    void mutate(std::vector<std::uint8_t>& data) {
        const std::size_t times = 1 + pick(4);
        for (std::size_t t = 0; t < times; ++t) {
            once(data);
        }
    }

private:
    void once(std::vector<std::uint8_t>& data) {
        // Values that mean something to a parser: terminators, padding, OSC's own markers,
        // the ends of a signed byte, a UTF-8 lead and continuation, a quote and a brace.
        static constexpr std::array<std::uint8_t, 14> kBytes = {
            0x00, 0xFF, 0x7F, 0x80, ',', '/', '#', '"', '{', ']', 0xC0, 0xBF, 0x0A, 'i'};
        // And words: lengths and counts made zero, huge, negative or one past a boundary.
        static constexpr std::array<std::uint32_t, 8> kWords = {
            0U, 0xFFFFFFFFU, 0x7FFFFFFFU, 0x80000000U, 0x00001000U, 0x0000FFFFU, 0x00010000U, 4U};
        const std::size_t size = data.size();
        switch (pick(8)) {
        case 0:
            if (size > 0) {
                const std::size_t at = pick(size);
                data[at] = static_cast<std::uint8_t>(data[at] ^ (1U << pick(8)));
            }
            break;
        case 1:
            if (size > 0) {
                data[pick(size)] = kBytes[pick(kBytes.size())];
            }
            break;
        case 2: {
            const std::size_t at = pick(size + 1);
            const std::size_t count = 1 + pick(8);
            std::vector<std::uint8_t> extra(count);
            for (std::uint8_t& byte : extra) {
                byte =
                    pick(2) == 0 ? kBytes[pick(kBytes.size())] : static_cast<std::uint8_t>(next());
            }
            data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), extra.begin(), extra.end());
            break;
        }
        case 3:
            if (size > 0) {
                const std::size_t at = pick(size);
                const std::size_t count = std::min(size - at, 1 + pick(16));
                data.erase(data.begin() + static_cast<std::ptrdiff_t>(at),
                           data.begin() + static_cast<std::ptrdiff_t>(at + count));
            }
            break;
        case 4:
            data.resize(pick(size + 1));
            break;
        case 5:
            if (size > 1) {
                const std::size_t from = pick(size);
                const std::size_t count = std::min(size - from, 1 + pick(32));
                const std::vector<std::uint8_t> chunk(
                    data.begin() + static_cast<std::ptrdiff_t>(from),
                    data.begin() + static_cast<std::ptrdiff_t>(from + count));
                const std::size_t to = pick(size);
                for (std::size_t i = 0; i < chunk.size() && to + i < data.size(); ++i) {
                    data[to + i] = chunk[i];
                }
            }
            break;
        case 6:
            if (size >= 4) {
                const std::size_t at = pick(size - 3);
                const std::uint32_t word = kWords[pick(kWords.size())];
                for (std::size_t i = 0; i < 4; ++i) {
                    data[at + i] =
                        static_cast<std::uint8_t>(word >> (8U * (pick(2) == 0 ? i : 3 - i)));
                }
            }
            break;
        default:
            // A run of one byte: the shape of a long string, a deep nest or a big number.
            if (size > 0) {
                const std::size_t at = pick(size);
                const std::uint8_t byte = kBytes[pick(kBytes.size())];
                data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), 1 + pick(200), byte);
            }
            break;
        }
    }

    std::uint32_t state_;
};

std::vector<std::uint8_t> bytesOf(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string textOf(const std::vector<std::uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}

bool isUtf8(std::string_view text) {
    return takt4::io::validUtf8(text) == text;
}

/// Every word of a definition read, and of the profile made from it: what the patch editor and
/// the library show, which Slint will not draw if it is not UTF-8.
bool showable(const takt4::fixtures::Definition& definition) {
    const auto all = [](const std::vector<std::string>& texts) {
        return std::all_of(texts.begin(), texts.end(),
                           [](const std::string& t) { return isUtf8(t); });
    };
    if (!isUtf8(definition.manufacturer) || !isUtf8(definition.model) ||
        !isUtf8(definition.revision) || !all(definition.notes)) {
        return false;
    }
    for (const auto& mode : definition.modes) {
        if (!isUtf8(mode.name) || !isUtf8(mode.refusal) || !all(mode.notes)) {
            return false;
        }
    }
    const takt4::fixtures::FixtureProfile profile = takt4::fixtures::mapDefinition(definition);
    if (!isUtf8(profile.manufacturer) || !isUtf8(profile.model) || !all(profile.notes)) {
        return false;
    }
    for (const auto& mode : profile.modes) {
        if (!isUtf8(mode.name) || !isUtf8(mode.refused) || !all(mode.notes)) {
            return false;
        }
        for (const auto& part : mode.parts) {
            for (const auto& channel : part) {
                if (!isUtf8(channel.label) || !isUtf8(channel.note)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// Whether `inner` lies wholly inside `outer`, compared as addresses.
bool inside(std::string_view inner, std::span<const std::byte> outer) {
    if (inner.empty()) {
        return true;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(outer.data());
    const auto end = begin + outer.size();
    const auto at = reinterpret_cast<std::uintptr_t>(inner.data());
    return at >= begin && at + inner.size() <= end;
}

/// Valid JSON with the wrong things in it: a value of any part of the document swapped for one of
/// another type or out of every range, a member taken out, an element repeated. What a file
/// edited by hand, or written by another program, can hold — past the parser, into each field's
/// own checks, where bytes mangled at random rarely get.
class JsonMutator {
public:
    explicit JsonMutator(Mutator& random) : random_(random) {}

    void mutate(nlohmann::json& document) {
        const std::size_t times = 1 + random_.pick(3);
        for (std::size_t t = 0; t < times; ++t) {
            std::vector<nlohmann::json::json_pointer> paths;
            gather(document, nlohmann::json::json_pointer(), paths);
            if (paths.empty()) {
                return;
            }
            const nlohmann::json::json_pointer path = paths[random_.pick(paths.size())];
            nlohmann::json& node = document[path];
            switch (random_.pick(4)) {
            case 0:
            case 1:
                node = hostile();
                break;
            case 2: {
                // Out of whatever holds it.
                nlohmann::json& parent = document[path.parent_pointer()];
                if (parent.is_object()) {
                    parent.erase(path.back());
                } else if (parent.is_array() && !parent.empty()) {
                    parent.erase(parent.begin() +
                                 static_cast<std::ptrdiff_t>(random_.pick(parent.size())));
                }
                break;
            }
            default:
                if (node.is_array() && !node.empty()) {
                    // An element twice: an id, a name or a port said by two of them. Copied first:
                    // the push may move the array the element is in.
                    const nlohmann::json twice = node[random_.pick(node.size())];
                    node.push_back(twice);
                } else {
                    node = hostile();
                }
                break;
            }
        }
    }

private:
    static void gather(const nlohmann::json& node, const nlohmann::json::json_pointer& at,
                       std::vector<nlohmann::json::json_pointer>& out) {
        if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                const auto child = at / it.key();
                out.push_back(child);
                gather(it.value(), child, out);
            }
        } else if (node.is_array()) {
            for (std::size_t i = 0; i < node.size(); ++i) {
                const auto child = at / i;
                out.push_back(child);
                gather(node[i], child, out);
            }
        }
    }

    nlohmann::json hostile() {
        switch (random_.pick(15)) {
        case 0:
            return nullptr;
        case 1:
            return true;
        case 2:
            return -1;
        case 3:
            return 0;
        case 4:
            return 65536;
        case 5:
            return 4294967296LL;
        case 6:
            return std::numeric_limits<std::int64_t>::min();
        case 7:
            return 1e308;
        case 8:
            return -0.5;
        case 9:
            return "";
        case 10:
            return std::string(4000, 'A');
        case 11:
            return std::string("\0\xE2\x80\xAE/../%s", 9);
        case 12:
            return nlohmann::json::array();
        case 13:
            return nlohmann::json::object();
        default:
            return nlohmann::json::array({1, "x", nullptr, nlohmann::json::object()});
        }
    }

    Mutator& random_;
};

std::vector<std::uint8_t> packetOf(takt4::output::OscMessage& message) {
    const std::span<const std::byte> packet = message.packet();
    std::vector<std::uint8_t> out(packet.size());
    std::memcpy(out.data(), packet.data(), packet.size());
    return out;
}

/// "#bundle", a time tag, and each element as its size and its bytes.
std::vector<std::uint8_t> bundleOf(const std::vector<std::vector<std::uint8_t>>& elements) {
    std::vector<std::uint8_t> out = bytesOf(std::string_view("#bundle\0", 8));
    for (int i = 0; i < 7; ++i) {
        out.push_back(0);
    }
    out.push_back(1);
    for (const auto& element : elements) {
        const auto size = static_cast<std::uint32_t>(element.size());
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(size >> shift));
        }
        out.insert(out.end(), element.begin(), element.end());
    }
    return out;
}

} // namespace

TEST_CASE("mangled OSC is read or refused, and what is read lies inside the datagram",
          "[fuzz][control]") {
    using takt4::output::OscMessage;
    std::vector<std::vector<std::uint8_t>> corpus;
    {
        OscMessage tap("/takt4/ctl/tap");
        corpus.push_back(packetOf(tap));
        OscMessage add("/takt4/ctl/output/add");
        add.addString("wall").addString("osc").addString("192.168.1.40").addInt(7000).addInt(12);
        corpus.push_back(packetOf(add));
        OscMessage many("/takt4/ctl/audio/channel");
        many.addInt(3).addFloat(0.5f).addString("").addInt(-1);
        corpus.push_back(packetOf(many));
    }
    corpus.push_back(bundleOf({corpus[0], corpus[1]}));
    corpus.push_back(bundleOf({bundleOf({corpus[2]}), corpus[0]}));

    Mutator mutator(1);
    const std::uint32_t count = rounds(20000);
    std::uint32_t read = 0;
    for (std::uint32_t round = 0; round < count; ++round) {
        std::vector<std::uint8_t> bytes = corpus[mutator.pick(corpus.size())];
        mutator.mutate(bytes);
        const std::span<const std::byte> packet(reinterpret_cast<const std::byte*>(bytes.data()),
                                                bytes.size());
        if (const auto view = takt4::control::parseOsc(packet)) {
            ++read;
            REQUIRE(view->argumentCount() <= takt4::control::OscView::kMaxArguments);
            REQUIRE_FALSE(view->address().empty());
            REQUIRE(view->address().front() == '/');
            REQUIRE(inside(view->address(), packet));
            for (std::size_t i = 0; i < view->argumentCount(); ++i) {
                REQUIRE(inside(view->argument(i).asString, packet));
            }
        }
        (void)takt4::control::forEachOscMessage(packet, [&](std::span<const std::byte> message) {
            const auto begin = reinterpret_cast<std::uintptr_t>(packet.data());
            const auto at = reinterpret_cast<std::uintptr_t>(message.data());
            REQUIRE(
                (message.empty() || (at >= begin && at + message.size() <= begin + packet.size())));
            (void)takt4::control::parseOsc(message);
        });
    }
    INFO(read << " of " << count << " still read as messages");
    CHECK(read > 0); // the mangling left some intact: the readers' happy paths ran too
}

TEST_CASE("mangled Pro DJ Link datagrams are read or refused", "[fuzz][prolink]") {
    std::vector<std::vector<std::uint8_t>> corpus;
    for (const char* file : {"cold-boot.txt", "joins-occupied.txt", "load-and-play.txt",
                             "master-handoff.txt", "nexus-and-djm.txt"}) {
        for (const auto& captured : takt4::testing::loadCapture(file)) {
            corpus.push_back(captured.bytes);
        }
    }
    REQUIRE(corpus.size() > 50);
    namespace prolink = takt4::timing::prolink;
    Mutator mutator(2);
    const std::uint32_t count = rounds(20000);
    std::uint32_t read = 0;
    for (std::uint32_t round = 0; round < count; ++round) {
        std::vector<std::uint8_t> bytes = corpus[mutator.pick(corpus.size())];
        mutator.mutate(bytes);
        prolink::AnnouncePacket announce;
        prolink::BeatPacket beat;
        prolink::StatusPacket status;
        read += prolink::parseAnnounce(bytes, announce) ? 1U : 0U;
        read += prolink::parseBeat(bytes, beat) ? 1U : 0U;
        read += prolink::parseStatus(bytes, status) ? 1U : 0U;
    }
    INFO(read << " of " << count << " still read as packets");
    CHECK(read > 0);
}

TEST_CASE("mangled MIDI is read or refused", "[fuzz][control]") {
    Mutator mutator(3);
    const std::uint32_t count = rounds(20000);
    for (std::uint32_t round = 0; round < count; ++round) {
        std::vector<std::uint8_t> bytes = {0x90, 36, 100};
        mutator.mutate(bytes);
        (void)takt4::control::readMidiEvent(bytes);
    }
    SUCCEED();
}

TEST_CASE("a mangled GDTF archive or description is read or refused with a reason, and what is "
          "read is safe to show",
          "[fuzz][fixtures][gdtf]") {
    using takt4::test::gdtfChannel;
    using takt4::test::gdtfDocument;
    using takt4::test::gdtfMode;
    const std::string xml = gdtfDocument(
        R"(<Geometry Name="Body"><Beam Name="Beam"/></Geometry>)",
        gdtfMode("Mode 1", "Body",
                 gdtfChannel("Body", "1", "Dimmer") + gdtfChannel("Body", "2", "ColorAdd_R") +
                     gdtfChannel("Body", "3", "ColorAdd_G") +
                     gdtfChannel("Body", "4", "ColorAdd_B")) +
            gdtfMode("Mode 2", "Body",
                     gdtfChannel("Body", "1,2", "Pan") + gdtfChannel("Body", "3", "Shutter1")));
    const std::vector<std::uint8_t> deflated = takt4::test::zipOf({{"description.xml", xml}}, true);
    const std::vector<std::uint8_t> stored = takt4::test::zipOf({{"description.xml", xml}}, false);
    REQUIRE(takt4::fixtures::readGdtfArchive(deflated, "a.gdtf").definition.has_value());

    Mutator mutator(4);
    const std::uint32_t count = rounds(3000);
    std::uint32_t read = 0;
    for (std::uint32_t round = 0; round < count; ++round) {
        takt4::fixtures::ReadResult result;
        const std::size_t which = mutator.pick(3);
        if (which == 2) {
            std::vector<std::uint8_t> text = bytesOf(xml);
            mutator.mutate(text);
            result = takt4::fixtures::readGdtfDescription(textOf(text), "a.gdtf");
        } else {
            std::vector<std::uint8_t> archive = which == 0 ? deflated : stored;
            mutator.mutate(archive);
            result = takt4::fixtures::readGdtfArchive(archive, "a.gdtf");
        }
        if (!result.definition) {
            REQUIRE_FALSE(result.problem.empty());
            REQUIRE(isUtf8(result.problem));
            continue;
        }
        ++read;
        REQUIRE(showable(*result.definition));
    }
    INFO(read << " of " << count << " still read");
    CHECK(read > 0);
}

TEST_CASE("a mangled Open Fixture Library file is read or refused with a reason, and what is read "
          "is safe to show",
          "[fuzz][fixtures][ofl]") {
    // The definitions the tests carry, a matrix, switching channels and wheels among them.
    std::vector<std::pair<std::filesystem::path, std::string>> corpus;
    const std::filesystem::path root = takt4::test::oflFile("");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        corpus.emplace_back(entry.path(), std::string(std::istreambuf_iterator<char>(in), {}));
    }
    REQUIRE(corpus.size() >= 5);

    Mutator mutator(5);
    const std::uint32_t count = rounds(3000);
    std::uint32_t read = 0;
    for (std::uint32_t round = 0; round < count; ++round) {
        const auto& [path, json] = corpus[mutator.pick(corpus.size())];
        std::vector<std::uint8_t> text = bytesOf(json);
        mutator.mutate(text);
        const takt4::fixtures::ReadResult result = takt4::fixtures::readOflText(
            textOf(text), path.filename().string(), path.parent_path().filename().string(),
            path.stem().string());
        if (!result.definition) {
            REQUIRE_FALSE(result.problem.empty());
            REQUIRE(isUtf8(result.problem));
            continue;
        }
        ++read;
        REQUIRE(showable(*result.definition));
    }
    INFO(read << " of " << count << " still read");
    CHECK(read > 0);

    // And valid JSON with the wrong things in it.
    JsonMutator structure(mutator);
    std::uint32_t structured = 0;
    for (std::uint32_t round = 0; round < count; ++round) {
        const auto& [path, json] = corpus[mutator.pick(corpus.size())];
        nlohmann::json document = nlohmann::json::parse(json);
        structure.mutate(document);
        const takt4::fixtures::ReadResult result = takt4::fixtures::readOflText(
            document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
            path.filename().string(), path.parent_path().filename().string(), path.stem().string());
        if (!result.definition) {
            REQUIRE_FALSE(result.problem.empty());
            continue;
        }
        ++structured;
        REQUIRE(showable(*result.definition));
    }
    INFO(structured << " of " << count << " still read after a change of structure");
    CHECK(structured > count / 10);
}

TEST_CASE(
    "a mangled settings file loads or is called damaged, and what loads saves and loads again",
    "[fuzz][settings]") {
    // A file with something in every part of it: the machine's, the outputs of each kind, rules,
    // a patched fixture.
    takt4::settings::Settings rich;
    rich.machine.deviceName = "MOTU Pro Audio";
    rich.machine.hostApiName = "ASIO";
    rich.machine.midiControlPort = "MOTU Pro Audio Midi In 1";
    rich.machine.midiBindings = {"note 36 ch 10 -> tap", "cc 64 ch 1 -> lock"};
    rich.machine.oscControlEnabled = true;
    rich.machine.oscRemoteFrom = "192.168.1.60";
    rich.preset.outputs = takt4::output::oscOutputs({{"192.168.1.40", 7000}, {"127.0.0.1", 7001}});
    takt4::output::OutputTarget clock;
    clock.name = "drums";
    clock.kind = takt4::output::OutputTarget::Kind::MidiClock;
    clock.device = "Microsoft GS Wavetable Synth 0";
    rich.preset.outputs.push_back(clock);
    takt4::output::OutputTarget lights;
    lights.name = "lights";
    lights.kind = takt4::output::OutputTarget::Kind::ArtNet;
    lights.host = "2.0.0.10";
    lights.port = 6454;
    rich.preset.outputs.push_back(lights);
    REQUIRE(takt4::output::ensureLinkOutput(rich.preset.outputs, true));
    takt4::output::ensureOutputIds(rich.preset.outputs);
    for (const char* id : {"strobe", "drop"}) {
        takt4::trigger::Rule::Config rule;
        rule.id = id;
        rule.name = std::string("the ") + id;
        rule.address = "/composition/layers/1/clips/1/connect";
        rule.every = 4;
        rich.preset.rules.push_back(rule);
    }
    takt4::dmx::Fixture par;
    par.id = "f-1";
    par.name = "par";
    par.group = "washes";
    par.address = 17;
    par.channels = {takt4::dmx::Role::Dimmer, takt4::dmx::Role::Red, takt4::dmx::Role::Green,
                    takt4::dmx::Role::Blue};
    par.parked = {0, 0, 0, 0};
    par.labels = {"dimmer", "red", "green", "blue"};
    rich.preset.fixtures.push_back(par);
    const std::string written = takt4::settings::toJson(rich);
    std::string problem;
    REQUIRE(takt4::settings::parse(written, problem).has_value());

    Mutator mutator(6);
    const std::uint32_t count = rounds(3000);
    std::uint32_t loaded = 0;
    for (std::uint32_t round = 0; round < count; ++round) {
        std::vector<std::uint8_t> text = bytesOf(written);
        mutator.mutate(text);
        problem.clear();
        const auto settings = takt4::settings::parse(textOf(text), problem);
        if (!settings) {
            REQUIRE_FALSE(problem.empty());
            REQUIRE(isUtf8(problem));
            continue;
        }
        ++loaded;
        // What loaded is written as a file takt4 reads back: never half a file, never one only
        // the mangling could have made.
        const std::string again = takt4::settings::toJson(*settings);
        REQUIRE(isUtf8(again));
        std::string reason;
        REQUIRE(takt4::settings::parse(again, reason).has_value());
    }
    INFO(loaded << " of " << count << " still loaded");
    CHECK(loaded > 0);

    // And valid JSON with the wrong things in it, which reaches each field's own checks: what
    // loads holds only what takt4 itself would write, so saving it and loading it again gives
    // the same settings — nothing drifts from save to save.
    JsonMutator structure(mutator);
    std::uint32_t structured = 0;
    const nlohmann::json original = nlohmann::json::parse(written);
    for (std::uint32_t round = 0; round < count; ++round) {
        nlohmann::json document = original;
        structure.mutate(document);
        problem.clear();
        const auto settings = takt4::settings::parse(
            document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), problem);
        if (!settings) {
            REQUIRE_FALSE(problem.empty());
            continue;
        }
        ++structured;
        const std::string again = takt4::settings::toJson(*settings);
        std::string reason;
        const auto reloaded = takt4::settings::parse(again, reason);
        REQUIRE(reloaded.has_value());
        // Less what is never written by design: what the file held (`inFile`, `stereoFromMono`)
        // and the names a newer takt4 wrote (`unknown`), which are notes about the reading.
        takt4::settings::Settings first = *settings;
        takt4::settings::Settings second = *reloaded;
        for (takt4::settings::Settings* one : {&first, &second}) {
            one->machine.inFile = false;
            one->machine.stereoFromMono = false;
            for (auto& rule : one->preset.rules) {
                rule.unknown.clear();
            }
        }
        CHECK(second == first);
    }
    INFO(structured << " of " << count << " still loaded after a change of structure");
    CHECK(structured > count / 2);
}
