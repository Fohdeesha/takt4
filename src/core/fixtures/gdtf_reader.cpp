#include "core/fixtures/gdtf_reader.hpp"

#include "core/fixtures/gdtf_attributes.hpp"
#include "core/fixtures/text.hpp"
#include "core/fixtures/zip_reader.hpp"
#include "core/io/utf8.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <pugixml.hpp>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace takt4::fixtures {

namespace gdtf {

std::optional<std::uint64_t> parseDmxValue(std::string_view text, unsigned bytes, bool& clamped) {
    clamped = false;
    text = text::trim(text);
    if (bytes < 1 || bytes > 8 || text.empty()) {
        return std::nullopt;
    }
    std::string_view number = text;
    unsigned width = 1;
    bool shifting = false;
    if (const std::size_t slash = text.find('/'); slash != std::string_view::npos) {
        number = text::trim(text.substr(0, slash));
        std::string_view suffix = text::trim(text.substr(slash + 1));
        if (!suffix.empty() && (suffix.back() == 's' || suffix.back() == 'S')) {
            shifting = true;
            suffix.remove_suffix(1);
        }
        const std::optional<std::int64_t> n = text::parseInteger(suffix);
        if (!n || *n < 1 || *n > 8) {
            return std::nullopt;
        }
        width = static_cast<unsigned>(*n);
    }
    if (number.empty() || number.front() == '-' || number.front() == '+') {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const char* end = number.data() + number.size();
    const auto [stop, error] = std::from_chars(number.data(), end, value);
    if (stop != end || (error != std::errc{} && error != std::errc::result_out_of_range)) {
        return std::nullopt;
    }
    const std::uint64_t largest = width >= 8 ? std::numeric_limits<std::uint64_t>::max()
                                             : (std::uint64_t{1} << (8 * width)) - 1;
    if (error == std::errc::result_out_of_range || value > largest) {
        value = largest;
        clamped = true;
    }
    if (width == bytes) {
        return value;
    }
    if (width > bytes) {
        return value >> (8 * (width - bytes));
    }
    if (shifting) {
        return value << (8 * (bytes - width));
    }
    // Mirroring: the low byte repeated into every byte the value does not have.
    const std::uint64_t low = value & 0xFFu;
    for (unsigned i = width; i < bytes; ++i) {
        value = (value << 8) | low;
    }
    return value;
}

} // namespace gdtf

namespace {

using pugi::xml_node;

/// Table 35's geometry types: every element that is a geometry, at any depth.
constexpr std::array<std::string_view, 18> kGeometryElements{"Geometry",
                                                             "Axis",
                                                             "FilterBeam",
                                                             "FilterColor",
                                                             "FilterGobo",
                                                             "FilterShaper",
                                                             "Beam",
                                                             "MediaServerLayer",
                                                             "MediaServerCamera",
                                                             "MediaServerMaster",
                                                             "Display",
                                                             "GeometryReference",
                                                             "Laser",
                                                             "WiringObject",
                                                             "Inventory",
                                                             "Structure",
                                                             "Support",
                                                             "Magnet"};

bool isGeometry(xml_node node) {
    if (node.type() != pugi::node_element) {
        return false;
    }
    const std::string_view name = node.name();
    return std::find(kGeometryElements.begin(), kGeometryElements.end(), name) !=
           kGeometryElements.end();
}

bool isReference(xml_node node) {
    return std::string_view(node.name()) == "GeometryReference";
}

std::string_view valueOf(xml_node node, const char* name) {
    return node.attribute(name).value();
}

bool hasAttribute(xml_node node, const char* name) {
    return static_cast<bool>(node.attribute(name));
}

std::vector<xml_node> elementsNamed(xml_node node, const char* name) {
    std::vector<xml_node> out;
    for (xml_node child : node.children(name)) {
        out.push_back(child);
    }
    return out;
}

std::vector<xml_node> geometryChildren(xml_node node) {
    std::vector<xml_node> out;
    for (xml_node child : node.children()) {
        if (isGeometry(child)) {
            out.push_back(child);
        }
    }
    return out;
}

/// Every geometry of the file, each with the top-level geometry it is under.
struct GeometryIndex {
    struct Entry {
        xml_node node;
        std::size_t top = 0;
        /// Inside a `GeometryReference`, whose only children per §10.14 are breaks — a geometry
        /// here is the file's error, and nothing places a channel through it.
        bool underReference = false;
    };
    std::vector<xml_node> tops;
    std::vector<Entry> all;
    /// The first geometry of each name, in document order: names are unique per the spec, and a
    /// duplicate resolves to the first.
    std::unordered_map<std::string, std::size_t> byName;

    explicit GeometryIndex(xml_node geometries) {
        for (xml_node top : geometryChildren(geometries)) {
            const std::size_t topIndex = tops.size();
            tops.push_back(top);
            // Depth first, in document order, with a stack of its own: a file nested ten thousand
            // deep is read as one, not as a crash.
            std::vector<std::pair<xml_node, bool>> stack{{top, false}};
            while (!stack.empty()) {
                const auto [node, under] = stack.back();
                stack.pop_back();
                all.push_back(Entry{node, topIndex, under});
                byName.emplace(std::string(valueOf(node, "Name")), all.size() - 1);
                const bool childUnder = under || isReference(node);
                const std::vector<xml_node> children = geometryChildren(node);
                for (auto it = children.rbegin(); it != children.rend(); ++it) {
                    stack.emplace_back(*it, childUnder);
                }
            }
        }
    }

    std::optional<std::size_t> topNamed(std::string_view name) const {
        for (std::size_t i = 0; i < tops.size(); ++i) {
            if (valueOf(tops[i], "Name") == name) {
                return i;
            }
        }
        return std::nullopt;
    }

    const Entry* find(std::string_view name) const {
        const auto found = byName.find(std::string(name));
        return found == byName.end() ? nullptr : &all[found->second];
    }
};

/// What one mode's geometry reaches: the names of the geometries in its tree, and the references
/// in it — not descending into a reference, whose children are breaks.
struct ModeTree {
    std::unordered_set<std::string> names;
    std::vector<xml_node> references;
    /// References with geometries inside them, which are ignored (the Rayzor 760 has 28).
    std::vector<std::string> nestedIgnored;
};

ModeTree treeOf(xml_node top) {
    ModeTree tree;
    std::vector<xml_node> stack{top};
    while (!stack.empty()) {
        const xml_node node = stack.back();
        stack.pop_back();
        if (isReference(node)) {
            tree.references.push_back(node);
            if (!geometryChildren(node).empty()) {
                tree.nestedIgnored.emplace_back(text::clean(valueOf(node, "Name")));
            }
            continue;
        }
        tree.names.emplace(valueOf(node, "Name"));
        const std::vector<xml_node> children = geometryChildren(node);
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
    return tree;
}

/// The top-level geometries a mode reaches only through a reference inside another referenced
/// geometry — what a channel there would need nested references for.
std::set<std::size_t> nestedTops(const GeometryIndex& index, const ModeTree& tree) {
    std::set<std::size_t> direct;
    for (xml_node reference : tree.references) {
        if (const auto top = index.topNamed(valueOf(reference, "Geometry"))) {
            direct.insert(*top);
        }
    }
    std::set<std::size_t> seen = direct;
    std::vector<std::size_t> queue(direct.begin(), direct.end());
    std::set<std::size_t> nested;
    while (!queue.empty()) {
        const std::size_t top = queue.back();
        queue.pop_back();
        for (xml_node reference : treeOf(index.tops[top]).references) {
            const auto target = index.topNamed(valueOf(reference, "Geometry"));
            if (target && seen.insert(*target).second) {
                nested.insert(*target);
                queue.push_back(*target);
            }
        }
    }
    return nested;
}

/// A DMXAddress — "37", or "2.5" for universe 2, address 5 — as one absolute number.
std::optional<std::int64_t> parseDmxAddress(std::string_view text) {
    text = text::trim(text);
    if (const std::size_t dot = text.find('.'); dot != std::string_view::npos) {
        const auto universe = text::parseInteger(text.substr(0, dot));
        const auto address = text::parseInteger(text.substr(dot + 1));
        if (!universe || !address || *universe < 1 || *universe > 1000000 || *address < 1 ||
            *address > 512) {
            return std::nullopt;
        }
        return (*universe - 1) * 512 + *address;
    }
    const auto plain = text::parseInteger(text);
    if (!plain || *plain < -1000000 || *plain > 1000000000) {
        return std::nullopt;
    }
    return *plain;
}

/// One channel function, with what this reader needs of it.
struct Function {
    xml_node node;
    std::string attribute;
    std::string name;
    std::uint64_t from = 0;
};

/// What a DMXChannel element is, whichever instance of it is being placed.
struct ChannelInfo {
    std::string attribute;
    Kind kind = Kind::Nothing;
    std::uint32_t ordinal = 0;
    std::string words;
    std::optional<std::uint64_t> defaultValue;
    std::uint64_t fallback = 0;
    std::optional<std::uint64_t> openValue;
    bool shared = false;
    std::string sharedWith;
};

/// The kinds whose channel is driven, and so whose range had better be all theirs.
bool isDrivable(Kind kind) {
    switch (kind) {
    case Kind::Dimmer:
    case Kind::Red:
    case Kind::Green:
    case Kind::Blue:
    case Kind::White:
    case Kind::WarmWhite:
    case Kind::CoolWhite:
    case Kind::Amber:
    case Kind::Uv:
    case Kind::CyanSub:
    case Kind::MagentaSub:
    case Kind::YellowSub:
    case Kind::OtherEmitter:
    case Kind::IndirectRed:
    case Kind::IndirectGreen:
    case Kind::IndirectBlue:
    case Kind::Pan:
    case Kind::Tilt:
    case Kind::Zoom:
    case Kind::Focus:
    case Kind::PanTiltSpeed:
        return true;
    default:
        return false;
    }
}

class ModeReader {
public:
    ModeReader(const GeometryIndex& index, xml_node mode) : index_(index), mode_(mode) {}

    DefMode read(std::string name) {
        DefMode out;
        out.name = std::move(name);
        out.refusal = place();
        if (out.refusal.empty()) {
            out.refusal = assemble(out);
        }
        if (!out.refusal.empty()) {
            out.parts.clear();
        }
        // Each said once, in the order it was met.
        std::vector<std::string> unique;
        for (std::string& note : notes_) {
            if (std::find(unique.begin(), unique.end(), note) == unique.end()) {
                unique.push_back(std::move(note));
            }
        }
        out.notes = std::move(unique);
        return out;
    }

private:
    struct Instance {
        xml_node channel;
        std::int64_t breakNumber = 1;
        std::vector<std::int64_t> addresses;
        std::string cell;
        std::string geometry;
        bool fromReference = false;
    };

    /// Every channel of the mode placed at its addresses — or why one cannot be.
    std::string place() {
        const std::string_view geometryName = valueOf(mode_, "Geometry");
        const auto top = index_.topNamed(geometryName);
        // **Every message here is for someone who has never read the GDTF spec** (the operator,
        // 2026-10-05: "simple concise sense to someone who may not be a dmx genius"). A geometry
        // is "a part of the fixture", a DMX break is "a start address", an offset is "an
        // address"; the file's own names stay, in quotes, for whoever opens it to look.
        if (!top) {
            return "the file doesn't describe the part of the fixture this mode is for (" +
                   quote(geometryName) + ")";
        }
        const ModeTree tree = treeOf(index_.tops[*top]);
        for (const std::string& reference : tree.nestedIgnored) {
            notes_.push_back("the file puts parts of the fixture inside " + quote(reference) +
                             ", where none belong — they were left out");
        }
        std::optional<std::set<std::size_t>> nested;

        for (xml_node channel : mode_.child("DMXChannels").children("DMXChannel")) {
            const std::string_view offsetText = text::trim(valueOf(channel, "Offset"));
            if (offsetText.empty() || text::equalsIgnoreCase(offsetText, "None")) {
                continue; // a virtual channel: takt4 has a virtual dimmer of its own
            }
            std::vector<std::int64_t> offsets;
            std::string_view rest = offsetText;
            while (true) {
                const std::size_t comma = rest.find(',');
                const auto offset = text::parseInteger(rest.substr(0, comma));
                if (!offset || *offset < -1000000 || *offset > 1000000000) {
                    return "the file gives channel " + describe(channel) +
                           " an address that isn't a number (" + quote(offsetText) + ")";
                }
                offsets.push_back(*offset);
                if (comma == std::string_view::npos) {
                    break;
                }
                rest = rest.substr(comma + 1);
            }
            if (offsets.size() > 8) {
                return "channel " + describe(channel) + " takes " + std::to_string(offsets.size()) +
                       " addresses; takt4 reads up to 8";
            }
            const std::string_view breakText = text::trim(valueOf(channel, "DMXBreak"));
            const bool overwrite = text::equalsIgnoreCase(breakText, "Overwrite");
            std::int64_t breakNumber = 1;
            if (!overwrite && !breakText.empty()) {
                const auto parsed = text::parseInteger(breakText);
                if (!parsed || *parsed < -1000000 || *parsed > 1000000) {
                    return "the file doesn't say which start address channel " + describe(channel) +
                           " is on";
                }
                breakNumber = *parsed;
            }
            const std::string geometry(valueOf(channel, "Geometry"));

            if (!overwrite && tree.names.contains(geometry)) {
                instances_.push_back(Instance{channel, breakNumber, offsets, {}, geometry, false});
                continue;
            }

            const GeometryIndex::Entry* entry = index_.find(geometry);
            std::vector<xml_node> references;
            if (entry != nullptr && !entry->underReference) {
                const std::string_view target = valueOf(index_.tops[entry->top], "Name");
                for (xml_node reference : tree.references) {
                    if (valueOf(reference, "Geometry") == target) {
                        references.push_back(reference);
                    }
                }
            }
            if (references.empty()) {
                if (entry != nullptr && !entry->underReference) {
                    if (!nested) {
                        nested = nestedTops(index_, tree);
                    }
                    if (nested->contains(entry->top)) {
                        return "a channel is on a part of the fixture nested inside another, "
                               "which takt4 can't place (" +
                               quote(geometry) + ")";
                    }
                }
                return "a channel is on a part of the fixture this mode doesn't reach (" +
                       quote(geometry) + ")";
            }
            for (xml_node reference : references) {
                const std::vector<xml_node> breaks = elementsNamed(reference, "Break");
                xml_node chosen;
                std::int64_t part = breakNumber;
                if (overwrite) {
                    if (!breaks.empty()) {
                        chosen = breaks.back();
                        const std::string_view own = text::trim(valueOf(chosen, "DMXBreak"));
                        const auto parsed =
                            own.empty() ? std::optional<std::int64_t>(1) : text::parseInteger(own);
                        part = parsed && *parsed >= -1000000 && *parsed <= 1000000 ? *parsed : 1;
                    }
                } else {
                    for (xml_node candidate : breaks) {
                        const std::string_view own = text::trim(valueOf(candidate, "DMXBreak"));
                        const auto parsed =
                            own.empty() ? std::optional<std::int64_t>(1) : text::parseInteger(own);
                        if (parsed && *parsed == breakNumber) {
                            chosen = candidate;
                            break;
                        }
                    }
                }
                std::int64_t shift = 0;
                if (!chosen) {
                    notes_.push_back("the file doesn't say where " +
                                     quote(valueOf(reference, "Name")) +
                                     " starts — its channels were placed from the first address");
                } else {
                    const std::string_view offsetAttribute = valueOf(chosen, "DMXOffset");
                    const auto offset = hasAttribute(chosen, "DMXOffset")
                                            ? parseDmxAddress(offsetAttribute)
                                            : std::optional<std::int64_t>(1);
                    if (!offset) {
                        notes_.push_back("the file gives " + quote(valueOf(reference, "Name")) +
                                         " a start that isn't an address — its channels were "
                                         "placed from the first address");
                    } else {
                        shift = *offset - 1;
                    }
                }
                Instance instance{channel,  part, offsets, text::clean(valueOf(reference, "Name")),
                                  geometry, true};
                for (std::int64_t& address : instance.addresses) {
                    address += shift;
                }
                instances_.push_back(std::move(instance));
            }
        }
        if (instances_.empty()) {
            return "the file lists no channels for it";
        }
        for (const Instance& instance : instances_) {
            for (const std::int64_t address : instance.addresses) {
                if (address < 1) {
                    return "the file puts channel " + describe(instance.channel) +
                           " before the fixture's first address";
                }
            }
        }
        return {};
    }

    /// The placed channels laid out as parts, each address read.
    std::string assemble(DefMode& out) {
        std::vector<std::int64_t> breaks;
        for (const Instance& instance : instances_) {
            if (std::find(breaks.begin(), breaks.end(), instance.breakNumber) == breaks.end()) {
                breaks.push_back(instance.breakNumber);
            }
        }
        std::sort(breaks.begin(), breaks.end());
        if (breaks.size() > 2) {
            return "it needs " + std::to_string(breaks.size()) +
                   " start addresses; takt4 handles 2 at most";
        }
        std::vector<std::int64_t> lengths(breaks.size(), 0);
        for (const Instance& instance : instances_) {
            const std::size_t part = partOf(breaks, instance.breakNumber);
            for (const std::int64_t address : instance.addresses) {
                lengths[part] = std::max(lengths[part], address);
            }
        }
        for (const std::int64_t length : lengths) {
            if (length > 512) {
                return "it needs " + std::to_string(length) +
                       " channels, more than a universe's 512";
            }
        }

        // Is there a plain Shutter(n) channel? Decides what a Shutter(n)Strobe… channel is.
        bool shutterInMode = false;
        for (const Instance& instance : instances_) {
            shutterInMode =
                shutterInMode || gdtf::isShutterAttribute(firstAttribute(instance.channel));
        }

        // A cell for a channel on a geometry of the mode's own tree only where the same attribute
        // is on several of them — "dimmer · Beam 1", "red · Segment1of4" — and not where there is
        // one: a pan is "pan", not "pan · Yoke".
        std::map<std::string, std::set<std::string>> geometriesOf;
        for (const Instance& instance : instances_) {
            if (!instance.fromReference) {
                geometriesOf[text::lowerAscii(firstAttribute(instance.channel))].insert(
                    instance.geometry);
            }
        }
        for (Instance& instance : instances_) {
            if (!instance.fromReference &&
                geometriesOf[text::lowerAscii(firstAttribute(instance.channel))].size() > 1) {
                instance.cell = text::clean(instance.geometry);
            }
        }

        struct Slot {
            const Instance* instance = nullptr;
            std::uint8_t byte = 0;
            std::vector<std::string> cells;
        };
        std::vector<std::vector<Slot>> slots(breaks.size());
        for (std::size_t part = 0; part < breaks.size(); ++part) {
            slots[part].resize(static_cast<std::size_t>(lengths[part]));
        }
        std::set<std::pair<std::size_t, std::int64_t>> reported;
        for (const Instance& instance : instances_) {
            const std::size_t part = partOf(breaks, instance.breakNumber);
            for (std::size_t k = 0; k < instance.addresses.size(); ++k) {
                const std::int64_t address = instance.addresses[k];
                Slot& slot = slots[part][static_cast<std::size_t>(address - 1)];
                if (slot.instance == nullptr) {
                    slot.instance = &instance;
                    slot.byte = static_cast<std::uint8_t>(k);
                    if (!instance.cell.empty()) {
                        slot.cells.push_back(instance.cell);
                    }
                    continue;
                }
                const bool same = static_cast<std::size_t>(slot.byte) == k &&
                                  text::equalsIgnoreCase(firstAttribute(slot.instance->channel),
                                                         firstAttribute(instance.channel));
                if (same) {
                    // One channel for several cells (CKC's Blinder WW2: one dimmer, two beams).
                    if (!instance.cell.empty() && std::find(slot.cells.begin(), slot.cells.end(),
                                                            instance.cell) == slot.cells.end()) {
                        slot.cells.push_back(instance.cell);
                    }
                } else if (reported.emplace(part, address).second) {
                    notes_.push_back("the file gives channel " + std::to_string(address) +
                                     (breaks.size() > 1 ? " of part " + std::to_string(part + 1)
                                                        : std::string()) +
                                     " two meanings — the first was kept");
                }
            }
        }

        out.parts.resize(breaks.size());
        for (std::size_t part = 0; part < breaks.size(); ++part) {
            out.parts[part].reserve(slots[part].size());
            for (const Slot& slot : slots[part]) {
                DefChannel channel;
                if (slot.instance == nullptr) {
                    channel.kind = Kind::Nothing;
                    channel.label = "(not in the file)";
                    out.parts[part].push_back(std::move(channel));
                    continue;
                }
                const auto bytes = static_cast<unsigned>(slot.instance->addresses.size());
                const ChannelInfo& info = infoOf(slot.instance->channel, bytes, shutterInMode);
                channel.kind = info.kind;
                channel.byte = slot.byte;
                channel.resolutionBytes = static_cast<std::uint8_t>(bytes);
                channel.cell = text::join(slot.cells, " + ");
                std::string label = info.words;
                if (!channel.cell.empty()) {
                    label += " · " + channel.cell;
                }
                if (slot.byte == 1) {
                    label += " fine";
                } else if (slot.byte > 1) {
                    label += " fine " + std::to_string(slot.byte);
                }
                channel.label = text::clean(label);
                channel.defaultValue = info.defaultValue;
                channel.fallbackValue = info.fallback;
                channel.openValue = info.openValue;
                channel.shared = info.shared;
                channel.sharedWith = info.sharedWith;
                channel.ordinal = info.ordinal;
                out.parts[part].push_back(std::move(channel));
            }
        }
        return {};
    }

    static std::size_t partOf(const std::vector<std::int64_t>& breaks, std::int64_t number) {
        return static_cast<std::size_t>(std::find(breaks.begin(), breaks.end(), number) -
                                        breaks.begin());
    }

    static std::string_view firstAttribute(xml_node channel) {
        return text::trim(valueOf(channel.child("LogicalChannel"), "Attribute"));
    }

    static std::string quote(std::string_view name) { return "\"" + text::clean(name) + "\""; }

    /// A channel as an operator can find it in the file: its attribute and geometry.
    static std::string describe(xml_node channel) {
        const std::string_view attribute = firstAttribute(channel);
        return quote(std::string(valueOf(channel, "Geometry")) + "_" + std::string(attribute));
    }

    std::optional<std::uint64_t> value(std::string_view text, unsigned bytes, xml_node channel) {
        bool clamped = false;
        const std::optional<std::uint64_t> parsed = gdtf::parseDmxValue(text, bytes, clamped);
        if (clamped) {
            notes_.push_back("the file gives channel " + describe(channel) +
                             " a value too large for it — read as its largest");
        }
        return parsed;
    }

    const ChannelInfo& infoOf(xml_node channel, unsigned bytes, bool shutterInMode) {
        const auto key = std::make_pair(channel.internal_object(), bytes);
        if (const auto found = infos_.find(key); found != infos_.end()) {
            return found->second;
        }
        ChannelInfo info;
        const std::vector<xml_node> logicals = elementsNamed(channel, "LogicalChannel");
        if (logicals.empty()) {
            info.kind = Kind::Nothing;
            info.words = "(no function in the file)";
            if (hasAttribute(channel, "Default")) {
                info.defaultValue = value(valueOf(channel, "Default"), bytes, channel);
            }
            return infos_.emplace(key, std::move(info)).first->second;
        }
        info.attribute = std::string(text::trim(valueOf(logicals.front(), "Attribute")));
        info.kind = gdtf::kindOfAttribute(info.attribute, shutterInMode, info.ordinal);
        info.words = info.attribute.empty() ? std::string("(no attribute in the file)")
                                            : gdtf::labelOfAttribute(info.attribute);

        // The first logical channel's functions — the others are alternatives to it — in DMX
        // order, ties in document order.
        std::vector<Function> functions;
        std::size_t number = 0;
        for (xml_node function : logicals.front().children("ChannelFunction")) {
            ++number;
            functions.push_back(functionOf(function, number, bytes, channel));
        }
        std::stable_sort(functions.begin(), functions.end(),
                         [](const Function& a, const Function& b) { return a.from < b.from; });

        // The initial function: named in full as "<geometry>_<first attribute>.<logical
        // attribute>.<function>", among every logical channel's functions; else the first
        // function of the first logical channel.
        const std::string_view wanted = valueOf(channel, "InitialFunction");
        const std::string prefix = std::string(valueOf(channel, "Geometry")) + "_" +
                                   std::string(valueOf(logicals.front(), "Attribute")) + ".";
        std::optional<Function> initial;
        if (!wanted.empty()) {
            for (xml_node logical : logicals) {
                std::size_t index = 0;
                for (xml_node function : logical.children("ChannelFunction")) {
                    ++index;
                    const Function candidate = functionOf(function, index, bytes, channel);
                    if (prefix + std::string(valueOf(logical, "Attribute")) + "." +
                            candidate.name ==
                        wanted) {
                        initial = candidate;
                        break;
                    }
                }
                if (initial) {
                    break;
                }
            }
        }
        if (!initial) {
            if (const xml_node first = logicals.front().child("ChannelFunction")) {
                initial = functionOf(first, 1, bytes, channel);
            }
        }
        if (initial && hasAttribute(initial->node, "Default")) {
            info.defaultValue = value(valueOf(initial->node, "Default"), bytes, channel);
        } else if (hasAttribute(channel, "Default")) {
            info.defaultValue = value(valueOf(channel, "Default"), bytes, channel);
        }
        info.fallback = initial ? initial->from : 0;

        if (isDrivable(info.kind)) {
            for (const Function& function : functions) {
                if (gdtf::isNoFeature(function.attribute)) {
                    continue;
                }
                std::uint32_t ordinal = 0;
                if (gdtf::kindOfAttribute(function.attribute, shutterInMode, ordinal) !=
                    info.kind) {
                    info.shared = true;
                    info.sharedWith = function.name.empty()
                                          ? gdtf::labelOfAttribute(function.attribute)
                                          : text::clean(function.name);
                    break;
                }
            }
        }
        if (info.kind == Kind::Shutter) {
            info.openValue = openValueOf(functions, info.defaultValue, bytes, channel);
        }
        return infos_.emplace(key, std::move(info)).first->second;
    }

    Function functionOf(xml_node function, std::size_t index, unsigned bytes, xml_node channel) {
        Function out;
        out.node = function;
        const std::string_view attribute = text::trim(valueOf(function, "Attribute"));
        out.attribute = attribute.empty() ? std::string("NoFeature") : std::string(attribute);
        // The spec's default name: the attribute and the function's number.
        out.name = hasAttribute(function, "Name") ? std::string(valueOf(function, "Name"))
                                                  : out.attribute + " " + std::to_string(index);
        const std::string_view from = valueOf(function, "DMXFrom");
        out.from = hasAttribute(function, "DMXFrom") ? value(from, bytes, channel).value_or(0) : 0;
        return out;
    }

    /// The value that opens a shutter, in the order measured on the GDTF Share examples: a channel
    /// set named "open" in a Shutter function; a Shutter function named so; one whose physical
    /// range is 1 to 1; the default when it lands in a "no feature" range.
    std::optional<std::uint64_t> openValueOf(const std::vector<Function>& functions,
                                             const std::optional<std::uint64_t>& defaultValue,
                                             unsigned bytes, xml_node channel) {
        for (const Function& function : functions) {
            if (!gdtf::isShutterAttribute(function.attribute)) {
                continue;
            }
            for (xml_node set : function.node.children("ChannelSet")) {
                if (text::hasWordOpen(valueOf(set, "Name"))) {
                    return hasAttribute(set, "DMXFrom")
                               ? value(valueOf(set, "DMXFrom"), bytes, channel).value_or(0)
                               : 0;
                }
            }
        }
        for (const Function& function : functions) {
            if (gdtf::isShutterAttribute(function.attribute) && text::hasWordOpen(function.name)) {
                return function.from;
            }
        }
        for (const Function& function : functions) {
            if (!gdtf::isShutterAttribute(function.attribute)) {
                continue;
            }
            const auto physicalFrom =
                hasAttribute(function.node, "PhysicalFrom")
                    ? text::parseNumber(valueOf(function.node, "PhysicalFrom"))
                    : std::optional<double>(0.0);
            const auto physicalTo = hasAttribute(function.node, "PhysicalTo")
                                        ? text::parseNumber(valueOf(function.node, "PhysicalTo"))
                                        : std::optional<double>(1.0);
            if (physicalFrom && physicalTo && *physicalFrom == 1.0 && *physicalTo == 1.0) {
                return function.from;
            }
        }
        if (defaultValue) {
            const Function* landsIn = nullptr;
            for (const Function& function : functions) {
                if (function.from <= *defaultValue) {
                    landsIn = &function;
                }
            }
            if (landsIn != nullptr && gdtf::isNoFeature(landsIn->attribute)) {
                return defaultValue;
            }
        }
        return std::nullopt;
    }

    const GeometryIndex& index_;
    xml_node mode_;
    std::vector<Instance> instances_;
    std::vector<std::string> notes_;
    std::map<std::pair<pugi::xml_node_struct*, unsigned>, ChannelInfo> infos_;
};

} // namespace

ReadResult readGdtfDescription(std::string_view xml, std::string_view fileName) {
    ReadResult result;
    if (xml.size() >= 3 && static_cast<unsigned char>(xml[0]) == 0xEF &&
        static_cast<unsigned char>(xml[1]) == 0xBB && static_cast<unsigned char>(xml[2]) == 0xBF) {
        xml.remove_prefix(3);
    }
    if (text::trim(xml).empty()) {
        result.problem = "the fixture description inside it is empty";
        return result;
    }
    pugi::xml_document document;
    const pugi::xml_parse_result parsed =
        document.load_buffer(xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
    if (!parsed) {
        result.problem = "the fixture description inside it is damaged (" +
                         std::string(parsed.description()) + " at byte " +
                         std::to_string(parsed.offset) + ")";
        return result;
    }
    const xml_node root = document.document_element();
    if (std::string_view(root.name()) != "GDTF") {
        result.problem = "it isn't a GDTF fixture file";
        return result;
    }

    Definition definition;
    definition.format = "gdtf";
    definition.fileName = text::clean(fileName, 260);

    // The version: 1.0, 1.1 and 1.2 as specified, a later 1.x as 1.2, any other major refused.
    const std::string_view version = text::trim(valueOf(root, "DataVersion"));
    const std::size_t dot = version.find('.');
    const auto major = text::parseInteger(version.substr(0, dot));
    const auto minor = dot == std::string_view::npos ? std::optional<std::int64_t>()
                                                     : text::parseInteger(version.substr(dot + 1));
    if (!major || !minor) {
        definition.notes.push_back("the file states no GDTF version — read as 1.2");
    } else if (*major != 1) {
        result.problem = "it is GDTF version " + text::clean(version) + "; takt4 reads version 1";
        return result;
    } else if (*minor < 0 || *minor > 2) {
        definition.notes.push_back("it is GDTF " + text::clean(version) +
                                   ", newer than takt4 knows — read as 1.2");
    }

    const xml_node type = root.child("FixtureType");
    if (!type) {
        result.problem = "it doesn't describe a fixture";
        return result;
    }
    definition.manufacturer = text::clean(valueOf(type, "Manufacturer"));
    for (const char* name : {"Name", "LongName", "ShortName"}) {
        definition.model = text::clean(valueOf(type, name));
        if (!definition.model.empty()) {
            break;
        }
    }
    const std::string id = text::upperAscii(text::clean(valueOf(type, "FixtureTypeID")));
    if (!id.empty()) {
        definition.key = id;
    } else {
        definition.key = "gdtf:" + definition.manufacturer + "/" + definition.model;
        definition.notes.push_back(
            "the file has no fixture ID — importing it again is matched by maker and model");
    }
    xml_node lastRevision;
    for (xml_node revision : type.child("Revisions").children("Revision")) {
        lastRevision = revision;
    }
    if (lastRevision) {
        definition.revision = text::clean(std::string(valueOf(lastRevision, "Date")) + " " +
                                          std::string(valueOf(lastRevision, "Text")));
    }

    const GeometryIndex index(type.child("Geometries"));
    std::map<std::string, int> seen;
    std::size_t number = 0;
    for (xml_node mode : type.child("DMXModes").children("DMXMode")) {
        ++number;
        std::string name = text::clean(valueOf(mode, "Name"));
        if (name.empty()) {
            name = "mode " + std::to_string(number);
        }
        // A name the file repeats is told apart in document order.
        const int count = ++seen[name];
        if (count > 1) {
            std::string unique;
            int suffix = count;
            do {
                unique = name + " (" + std::to_string(suffix++) + ")";
            } while (seen.contains(unique));
            seen[unique] = 1;
            name = unique;
        }
        definition.modes.push_back(ModeReader(index, mode).read(std::move(name)));
    }
    if (definition.modes.empty()) {
        result.problem = "it has no modes";
        return result;
    }
    result.definition = std::move(definition);
    return result;
}

ReadResult readGdtfArchive(std::span<const std::uint8_t> archive, std::string_view fileName) {
    ReadResult result;
    std::string problem;
    const std::optional<std::string> xml =
        readZipMember(archive, "description.xml", kGdtfDescriptionLimit, problem);
    if (!xml) {
        result.problem = problem;
        return result;
    }
    return readGdtfDescription(*xml, fileName);
}

ReadResult readGdtfFile(const std::filesystem::path& path) {
    ReadResult result;
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (!code && size > kGdtfArchiveLimit) {
        result.problem =
            "it is " + std::to_string(size / (1024 * 1024)) + " MB, too big to be a fixture file";
        return result;
    }
    std::string problem;
    const std::optional<std::string> xml =
        readZipMemberFromFile(path, "description.xml", kGdtfDescriptionLimit, problem);
    if (!xml) {
        result.problem = problem;
        return result;
    }
    return readGdtfDescription(*xml, io::pathText(path.filename()));
}

} // namespace takt4::fixtures
