#include "core/fixtures/ofl_reader.hpp"

#include "core/fixtures/natural_order.hpp"
#include "core/fixtures/text.hpp"
#include "core/io/utf8.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace takt4::fixtures {

namespace ofl {

namespace {

std::vector<std::uint8_t> bytesOf(std::uint64_t value, unsigned count) {
    std::vector<std::uint8_t> bytes(count, 0);
    for (unsigned i = 0; i < count; ++i) {
        bytes[count - 1 - i] = static_cast<std::uint8_t>(value & 0xFFu);
        value >>= 8;
    }
    return bytes;
}

std::uint64_t valueOf(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 0;
    for (const std::uint8_t byte : bytes) {
        value = (value << 8) | byte;
    }
    return value;
}

} // namespace

std::uint64_t scaleValue(std::uint64_t value, unsigned from, unsigned to) {
    if (from < 1 || to < 1 || from > 8 || to > 8) {
        return value;
    }
    std::vector<std::uint8_t> bytes = bytesOf(value, from);
    while (bytes.size() < to) {
        bytes.push_back(bytes.back());
    }
    bytes.resize(to);
    return valueOf(bytes);
}

std::pair<std::uint64_t, std::uint64_t> scaleRange(std::uint64_t start, std::uint64_t end,
                                                   unsigned from, unsigned to) {
    if (from < 1 || to < 1 || from > 8 || to > 8) {
        return {start, end};
    }
    std::vector<std::uint8_t> startBytes = bytesOf(start, from);
    std::vector<std::uint8_t> endBytes = bytesOf(end, from);
    while (endBytes.size() < to) {
        endBytes.push_back(255);
    }
    while (startBytes.size() < to) {
        startBytes.push_back(0);
    }
    endBytes.resize(to);
    // The end is cut first, then the start a byte at a time — as OFL does it, which is what
    // makes the rounding compare against the end as it will be.
    while (startBytes.size() > to) {
        const std::uint8_t cut = startBytes.back();
        startBytes.pop_back();
        if (cut > 0 && valueOf(startBytes) < valueOf(endBytes)) {
            startBytes = bytesOf(valueOf(startBytes) + 1, static_cast<unsigned>(startBytes.size()));
        }
    }
    return {valueOf(startBytes), valueOf(endBytes)};
}

} // namespace ofl

namespace {

using nlohmann::json;

/// Deeper than any fixture file nests (the schema reaches 7), and shallow enough that nothing
/// that walks the document can run out of stack.
constexpr std::size_t kDepthLimit = 64;
/// More pixels than any fixture has. A matrix of a million would be a million keys to sort.
constexpr std::size_t kPixelLimit = 10000;
/// A mode longer than a universe is refused; expanding one past this is never needed.
constexpr std::size_t kChannelLimit = 512;

/// Whether the JSON text nests deeper than `kDepthLimit`, counted outside strings.
bool nestsTooDeep(std::string_view text) {
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char c : text) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            if (++depth > kDepthLimit) {
                return true;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            --depth;
        }
    }
    return false;
}

std::string replaceAll(std::string_view text, std::string_view what, std::string_view with) {
    std::string out;
    std::size_t at = 0;
    while (true) {
        const std::size_t found = text.find(what, at);
        if (found == std::string_view::npos) {
            out.append(text.substr(at));
            return out;
        }
        out.append(text.substr(at, found - at));
        out.append(with);
        at = found + what.size();
    }
}

constexpr std::string_view kPixelVariable = "$pixelKey";

/// OFL's `TemplateChannel.resolveTemplateObject`: every string — keys and values — with
/// "$pixelKey" replaced.
json resolveTemplate(const json& value, std::string_view pixel) {
    if (value.is_string()) {
        return replaceAll(value.get_ref<const std::string&>(), kPixelVariable, pixel);
    }
    if (value.is_array()) {
        json out = json::array();
        for (const json& item : value) {
            out.push_back(resolveTemplate(item, pixel));
        }
        return out;
    }
    if (value.is_object()) {
        json out = json::object();
        for (const auto& [key, item] : value.items()) {
            out[replaceAll(key, kPixelVariable, pixel)] = resolveTemplate(item, pixel);
        }
        return out;
    }
    return value;
}

/// The pixel or group key `pattern` must be resolved with to give `key`, if any of `candidates`
/// does — the reverse of `resolveTemplateString`, without resolving every template for every
/// pixel to find out.
std::optional<std::string> pixelFor(std::string_view pattern, std::string_view key,
                                    const std::unordered_set<std::string>& candidates) {
    std::size_t occurrences = 0;
    for (std::size_t at = pattern.find(kPixelVariable); at != std::string_view::npos;
         at = pattern.find(kPixelVariable, at + kPixelVariable.size())) {
        ++occurrences;
    }
    if (occurrences == 0) {
        return std::nullopt;
    }
    const std::size_t fixed = pattern.size() - occurrences * kPixelVariable.size();
    if (key.size() < fixed || (key.size() - fixed) % occurrences != 0) {
        return std::nullopt;
    }
    const std::size_t length = (key.size() - fixed) / occurrences;
    const std::size_t first = pattern.find(kPixelVariable);
    if (first + length > key.size()) {
        return std::nullopt;
    }
    std::string pixel(key.substr(first, length));
    if (!candidates.contains(pixel) || replaceAll(pattern, kPixelVariable, pixel) != key) {
        return std::nullopt;
    }
    return pixel;
}

const json* member(const json& object, const char* name) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &*found;
}

std::string stringOf(const json& object, const char* name) {
    const json* value = member(object, name);
    return value != nullptr && value->is_string() ? value->get<std::string>() : std::string();
}

/// A non-negative whole number from JSON, or nothing.
std::optional<std::uint64_t> unsignedOf(const json& value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signedValue = value.get<std::int64_t>();
        return signedValue < 0 ? std::optional<std::uint64_t>()
                               : static_cast<std::uint64_t>(signedValue);
    }
    return std::nullopt;
}

std::uint64_t largestAt(unsigned bytes) {
    return bytes >= 8 ? ~std::uint64_t{0} : (std::uint64_t{1} << (8 * bytes)) - 1;
}

/// One capability, with what the reader needs of it.
struct Capability {
    std::string type;
    std::string color;
    std::string shutterEffect;
    std::string menuClick;
    bool hasRange = false;
    /// At the channel's `dmxValueResolution`.
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    std::vector<std::string> wheels;
    const json* switchChannels = nullptr;
    std::string helpWanted;
};

/// A channel object — an available channel, or a template channel resolved for one pixel —
/// with what the reader needs of it.
struct ChannelData {
    std::string key;
    std::string name;
    std::vector<std::string> fineAliases;
    unsigned maxResolution = 1;
    unsigned valueResolution = 1;
    std::optional<std::uint64_t> defaultRaw;
    bool defaultClamped = false;
    std::vector<Capability> capabilities;
};

/// What a channel's capabilities make of it, before the mode decides cyan, magenta and
/// yellow.
struct KindReading {
    Kind kind = Kind::Other;
    bool shared = false;
    std::string sharedWith;
    std::string severalThings;
};

/// One address of a mode as listed, resolved to a channel.
struct Entry {
    bool nothing = false;
    std::string label;
    std::string cell;
    /// The channel's own key (the coarse one's) — which `ChannelData` it is.
    std::string channel;
    /// Which cells are one function: a template's channels for every pixel count as one when
    /// ordinals are given out, as GDTF's instances of one channel do.
    std::string identity;
    std::uint8_t byte = 0;
    std::vector<std::string> notes;
};

/// Where a key resolved to, before it is an entry.
struct Resolution {
    std::string channel;
    std::string identity;
    std::uint8_t byte = 0;
    std::string pixel;
};

class Reader {
public:
    Reader(const json& root, const std::vector<std::string>& groupInsertionOrder) : root_(root) {
        available_ = member(root, "availableChannels");
        if (available_ != nullptr && !available_->is_object()) {
            available_ = nullptr;
        }
        templates_ = member(root, "templateChannels");
        if (templates_ != nullptr && !templates_->is_object()) {
            templates_ = nullptr;
        }
        readWheels();
        readMatrix(groupInsertionOrder);
        indexAvailable();
    }

    DefMode readMode(const json& mode, std::string name) {
        DefMode out;
        out.name = std::move(name);
        std::vector<Entry> entries;
        // Said for someone who has never opened a fixture file — see the GDTF reader's `place`.
        // A matrix is "pixels", a template channel "a per-pixel channel".
        out.refusal = listChannels(mode, entries);
        if (out.refusal.empty() && entries.size() > kChannelLimit) {
            out.refusal = "it needs " + std::to_string(entries.size()) +
                          " channels, more than a universe's 512";
        }
        if (out.refusal.empty() && entries.empty()) {
            out.refusal = "the file lists no channels for it";
        }
        if (out.refusal.empty()) {
            out.parts.push_back(assemble(entries));
        }
        return out;
    }

private:
    // --- the fixture's structure -------------------------------------------------------

    void readWheels() {
        const json* wheels = member(root_, "wheels");
        if (wheels == nullptr || !wheels->is_object()) {
            return;
        }
        for (const auto& [name, wheel] : wheels->items()) {
            const json* slots = member(wheel, "slots");
            if (slots == nullptr || !slots->is_array() || slots->empty()) {
                continue;
            }
            // OFL's `Wheel.type`: the most frequent slot type — of the types tied for most, the
            // one of the last such slot, as its stable sort and pop leave it — and any animation
            // gobo type as "AnimationGobo".
            std::vector<std::string> types;
            std::map<std::string, std::size_t> counts;
            for (const json& slot : *slots) {
                types.push_back(stringOf(slot, "type"));
                ++counts[types.back()];
            }
            std::size_t most = 0;
            for (const auto& [type, count] : counts) {
                most = std::max(most, count);
            }
            std::string type;
            for (const std::string& candidate : types) {
                if (counts[candidate] == most) {
                    type = candidate;
                }
            }
            wheelTypes_[name] = type.starts_with("AnimationGobo") ? "AnimationGobo" : type;
        }
    }

    void readMatrix(const std::vector<std::string>& groupInsertionOrder) {
        const json* matrix = member(root_, "matrix");
        if (matrix == nullptr) {
            return;
        }
        hasMatrix_ = true;
        if (!matrix->is_object()) {
            matrixProblem_ = "its pixel layout can't be read";
            return;
        }
        // pixelKeys by z, y, x, or generated from pixelCount — OFL's `Matrix`.
        std::vector<std::vector<std::vector<std::optional<std::string>>>> structure;
        std::size_t countX = 1;
        std::size_t countY = 1;
        std::size_t countZ = 1;
        if (const json* keys = member(*matrix, "pixelKeys"); keys != nullptr && keys->is_array()) {
            countZ = keys->size();
            std::size_t total = 0;
            for (const json& layer : *keys) {
                auto& outLayer = structure.emplace_back();
                if (!layer.is_array()) {
                    continue;
                }
                countY = std::max(countY, layer.size());
                for (const json& row : layer) {
                    auto& outRow = outLayer.emplace_back();
                    if (!row.is_array()) {
                        continue;
                    }
                    countX = std::max(countX, row.size());
                    for (const json& key : row) {
                        if (++total > kPixelLimit) {
                            matrixProblem_ =
                                "it has more than " + std::to_string(kPixelLimit) + " pixels";
                            return;
                        }
                        // As written: a key is matched byte for byte inside template names, so
                        // it is not trimmed (the JSON parser has already refused bad UTF-8).
                        outRow.push_back(key.is_string()
                                             ? std::optional<std::string>(key.get<std::string>())
                                             : std::nullopt);
                    }
                }
            }
        } else if (const json* count = member(*matrix, "pixelCount");
                   count != nullptr && count->is_array() && count->size() == 3) {
            std::array<std::uint64_t, 3> xyz{};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                const auto value = unsignedOf((*count)[axis]);
                if (!value || *value < 1 || *value > kPixelLimit) {
                    matrixProblem_ = "its pixel count can't be read";
                    return;
                }
                xyz[axis] = *value;
            }
            if (xyz[0] * xyz[1] * xyz[2] > kPixelLimit) {
                matrixProblem_ = "it has more than " + std::to_string(kPixelLimit) + " pixels";
                return;
            }
            countX = static_cast<std::size_t>(xyz[0]);
            countY = static_cast<std::size_t>(xyz[1]);
            countZ = static_cast<std::size_t>(xyz[2]);
            const bool definesX = countX > 1;
            const bool definesY = countY > 1;
            const bool definesZ = countZ > 1;
            const int axes = int{definesX} + int{definesY} + int{definesZ};
            if (axes == 0) {
                matrixProblem_ = "its pixel layout has a single pixel, which the file can't name";
                return;
            }
            for (std::size_t z = 1; z <= countZ; ++z) {
                auto& layer = structure.emplace_back();
                for (std::size_t y = 1; y <= countY; ++y) {
                    auto& row = layer.emplace_back();
                    for (std::size_t x = 1; x <= countX; ++x) {
                        std::string key;
                        if (axes == 1) {
                            key = std::to_string(std::max({x, y, z}));
                        } else if (axes == 2) {
                            const std::size_t first = definesX ? x : y;
                            const std::size_t last = definesY ? y : z;
                            key = "(" + std::to_string(first) + ", " + std::to_string(last) + ")";
                        } else {
                            key = "(" + std::to_string(x) + ", " + std::to_string(y) + ", " +
                                  std::to_string(z) + ")";
                        }
                        row.emplace_back(std::move(key));
                    }
                }
            }
        } else {
            matrixProblem_ = "its pixel layout lists no pixels";
            return;
        }
        if (countX * countY * countZ > kPixelLimit) {
            matrixProblem_ = "it has more than " + std::to_string(kPixelLimit) + " pixels";
            return;
        }
        // Each pixel's position, z, then y, then x; a key listed twice keeps its first place in
        // the order and the last position, as OFL's object of positions does.
        for (std::size_t z = 0; z < structure.size(); ++z) {
            for (std::size_t y = 0; y < structure[z].size(); ++y) {
                for (std::size_t x = 0; x < structure[z][y].size(); ++x) {
                    const std::optional<std::string>& key = structure[z][y][x];
                    if (!key) {
                        continue; // a hole
                    }
                    if (!positions_.contains(*key)) {
                        insertionOrder_.push_back(*key);
                    }
                    positions_[*key] = {x + 1, y + 1, z + 1};
                }
            }
        }
        pixelsAbc_ = natural::sorted(insertionOrder_);

        if (const json* groups = member(*matrix, "pixelGroups");
            groups != nullptr && groups->is_object()) {
            groupKeys_ = natural::objectKeyOrder(groupInsertionOrder);
            // A key the order recorder missed is still a group; it goes last.
            for (const auto& [key, group] : groups->items()) {
                if (std::find(groupKeys_.begin(), groupKeys_.end(), key) == groupKeys_.end()) {
                    groupKeys_.push_back(key);
                }
            }
            std::erase_if(groupKeys_,
                          [&](const std::string& key) { return !groups->contains(key); });
            for (const std::string& key : groupKeys_) {
                if (const json& group = groups->at(key); group.is_string() && group == "all") {
                    allGroups_.insert(key);
                }
            }
        }
        for (const std::string& key : pixelsAbc_) {
            candidates_.insert(key);
        }
        for (const std::string& key : groupKeys_) {
            candidates_.insert(key);
        }
    }

    /// The pixel keys `repeatFor` names, in its order — or why there are none.
    std::string repeatFor(const json& repeat, std::vector<std::string>& out) {
        if (repeat.is_array()) {
            for (const json& key : repeat) {
                if (!key.is_string()) {
                    return "its per-pixel channels name a pixel in a way takt4 can't read";
                }
                out.push_back(key.get<std::string>());
            }
            return {};
        }
        if (!repeat.is_string()) {
            return "its per-pixel channels don't say which pixels they repeat for";
        }
        const std::string keyword = repeat.get<std::string>();
        if (!hasMatrix_) {
            return "it repeats channels for each pixel, but the file describes no pixels";
        }
        if (!matrixProblem_.empty()) {
            return matrixProblem_;
        }
        if (keyword == "eachPixelABC") {
            out = pixelsAbc_;
            return {};
        }
        if (keyword == "eachPixelGroup") {
            out = groupKeys_;
            return {};
        }
        constexpr std::string_view stem = "eachPixel";
        if (keyword.size() == stem.size() + 3 && keyword.starts_with(stem)) {
            const std::string axes = keyword.substr(stem.size());
            std::array<std::size_t, 3> order{};
            std::set<char> seen;
            for (std::size_t i = 0; i < 3; ++i) {
                const char axis = axes[i];
                if ((axis != 'X' && axis != 'Y' && axis != 'Z') || !seen.insert(axis).second) {
                    return "its per-pixel channels repeat in an order takt4 doesn't know (" +
                           quote(keyword) + ")";
                }
                order[i] = static_cast<std::size_t>(axis - 'X');
            }
            // The last axis named is the most significant: "XYZ" reads like a book.
            out = pixelsAbc_;
            std::stable_sort(out.begin(), out.end(),
                             [&](const std::string& a, const std::string& b) {
                                 const auto& pa = positions_.at(a);
                                 const auto& pb = positions_.at(b);
                                 if (pa[order[2]] != pb[order[2]]) {
                                     return pa[order[2]] < pb[order[2]];
                                 }
                                 if (pa[order[1]] != pb[order[1]]) {
                                     return pa[order[1]] < pb[order[1]];
                                 }
                                 return pa[order[0]] < pb[order[0]];
                             });
            return {};
        }
        return "its per-pixel channels repeat in an order takt4 doesn't know (" + quote(keyword) +
               ")";
    }

    void indexAvailable() {
        if (available_ == nullptr) {
            return;
        }
        for (const auto& [key, channel] : available_->items()) {
            if (!channel.is_object()) {
                continue;
            }
            if (const json* aliases = member(channel, "fineChannelAliases");
                aliases != nullptr && aliases->is_array()) {
                for (std::size_t i = 0; i < aliases->size(); ++i) {
                    if ((*aliases)[i].is_string()) {
                        fineAliases_.emplace(
                            (*aliases)[i].get<std::string>(),
                            std::make_pair(
                                key, static_cast<std::uint8_t>(std::min<std::size_t>(i + 1, 255))));
                    }
                }
            }
            for (const std::string& alias : switchingAliasesOf(channel)) {
                switchingAliases_.emplace(alias, key);
            }
        }
    }

    /// The aliases a channel's capabilities switch — the keys of the first capability's
    /// `switchChannels`, which every capability of a switching channel lists.
    static std::vector<std::string> switchingAliasesOf(const json& channel) {
        std::vector<std::string> out;
        const json* first = member(channel, "capability");
        if (first == nullptr) {
            const json* capabilities = member(channel, "capabilities");
            if (capabilities != nullptr && capabilities->is_array() && !capabilities->empty()) {
                first = &(*capabilities)[0];
            }
        }
        if (first == nullptr) {
            return out;
        }
        if (const json* switches = member(*first, "switchChannels");
            switches != nullptr && switches->is_object()) {
            for (const auto& [alias, target] : switches->items()) {
                out.push_back(alias);
            }
        }
        return out;
    }

    // --- channels --------------------------------------------------------------------

    const ChannelData& data(const std::string& key, const json& object) {
        if (const auto found = channels_.find(key); found != channels_.end()) {
            return found->second;
        }
        ChannelData channel;
        channel.key = key;
        channel.name = stringOf(object, "name");
        if (channel.name.empty()) {
            channel.name = key;
        }
        if (const json* aliases = member(object, "fineChannelAliases");
            aliases != nullptr && aliases->is_array()) {
            for (const json& alias : *aliases) {
                channel.fineAliases.push_back(alias.is_string() ? alias.get<std::string>()
                                                                : std::string());
            }
        }
        channel.maxResolution =
            static_cast<unsigned>(std::min<std::size_t>(1 + channel.fineAliases.size(), 8));
        channel.valueResolution = channel.maxResolution;
        const std::string resolution = stringOf(object, "dmxValueResolution");
        if (resolution == "8bit") {
            channel.valueResolution = 1;
        } else if (resolution == "16bit") {
            channel.valueResolution = 2;
        } else if (resolution == "24bit") {
            channel.valueResolution = 3;
        }
        const std::uint64_t largest = largestAt(channel.valueResolution);
        if (const json* value = member(object, "defaultValue"); value != nullptr) {
            if (const auto plain = unsignedOf(*value)) {
                channel.defaultRaw = std::min(*plain, largest);
                channel.defaultClamped = *plain > largest;
            } else if (value->is_string()) {
                // A percentage of the value resolution's range: floor(p / 100 × (256^res − 1)).
                std::string_view percent = text::trim(value->get_ref<const std::string&>());
                if (percent.ends_with('%')) {
                    percent.remove_suffix(1);
                }
                if (const auto number = text::parseNumber(percent)) {
                    const double share = std::clamp(*number / 100.0, 0.0, 1.0);
                    channel.defaultRaw =
                        static_cast<std::uint64_t>(share * static_cast<double>(largest));
                }
            }
        }
        const auto readCapability = [&](const json& capability, bool single) {
            Capability out;
            out.type = stringOf(capability, "type");
            out.color = stringOf(capability, "color");
            out.shutterEffect = stringOf(capability, "shutterEffect");
            out.menuClick = stringOf(capability, "menuClick");
            out.helpWanted = stringOf(capability, "helpWanted");
            if (single) {
                out.hasRange = true;
                out.from = 0;
                out.to = largest;
            } else if (const json* range = member(capability, "dmxRange");
                       range != nullptr && range->is_array() && range->size() == 2) {
                const auto from = unsignedOf((*range)[0]);
                const auto to = unsignedOf((*range)[1]);
                if (from && to) {
                    out.hasRange = true;
                    out.from = std::min(*from, largest);
                    out.to = std::min(*to, largest);
                }
            }
            if (const json* wheel = member(capability, "wheel"); wheel != nullptr) {
                if (wheel->is_string()) {
                    out.wheels.push_back(wheel->get<std::string>());
                } else if (wheel->is_array()) {
                    for (const json& name : *wheel) {
                        out.wheels.push_back(name.is_string() ? name.get<std::string>()
                                                              : std::string());
                    }
                }
            } else if (out.type.find("Wheel") != std::string::npos) {
                out.wheels.push_back(channel.name); // OFL's default: the channel's own name
            }
            if (const json* switches = member(capability, "switchChannels");
                switches != nullptr && switches->is_object()) {
                out.switchChannels = switches;
            }
            channel.capabilities.push_back(std::move(out));
        };
        if (const json* single = member(object, "capability");
            single != nullptr && single->is_object()) {
            readCapability(*single, true);
        } else if (const json* capabilities = member(object, "capabilities");
                   capabilities != nullptr && capabilities->is_array()) {
            for (const json& capability : *capabilities) {
                if (capability.is_object()) {
                    readCapability(capability, false);
                }
            }
        }
        return channels_.emplace(key, std::move(channel)).first->second;
    }

    /// The JSON of a template channel resolved for a pixel — kept, since `ChannelData` points
    /// into it for switching.
    const json& resolvedTemplate(const std::string& templateKey, const std::string& pixel) {
        const std::string id = templateKey + '\n' + pixel;
        if (const auto found = resolved_.find(id); found != resolved_.end()) {
            return *found->second;
        }
        resolvedStore_.push_back(resolveTemplate(templates_->at(templateKey), pixel));
        return *resolved_.emplace(id, &resolvedStore_.back()).first->second;
    }

    /// A key resolved as OFL's `Mode` resolves one, after switching: an available channel, a fine
    /// alias, a resolved template channel.
    std::optional<Resolution> resolveKey(const std::string& key) {
        if (available_ != nullptr) {
            if (const auto found = available_->find(key);
                found != available_->end() && found->is_object()) {
                data(key, *found);
                // A resolved matrix channel the file overrides with an available one keeps the
                // matrix's pixel (OFL moves the override into the matrix channel's place).
                return Resolution{key, "a:" + key, 0, templatePixelOf(key).value_or("")};
            }
        }
        if (const auto found = fineAliases_.find(key); found != fineAliases_.end()) {
            const std::string& coarse = found->second.first;
            data(coarse, available_->at(coarse));
            return Resolution{coarse, "a:" + coarse, found->second.second,
                              templatePixelOf(coarse).value_or("")};
        }
        if (templates_ != nullptr && !candidates_.empty()) {
            // Last template first: OFL builds its channels in template order into one object,
            // so the last of two that resolve to one key is the one it keeps.
            const auto items = templates_->items();
            std::vector<std::pair<std::string, const json*>> list;
            for (const auto& [templateKey, channel] : items) {
                list.emplace_back(templateKey, &channel);
            }
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                const auto& [templateKey, channel] = *it;
                if (!channel->is_object()) {
                    continue;
                }
                if (const json* aliases = member(*channel, "fineChannelAliases");
                    aliases != nullptr && aliases->is_array()) {
                    for (std::size_t i = 0; i < aliases->size(); ++i) {
                        if (!(*aliases)[i].is_string()) {
                            continue;
                        }
                        if (const auto pixel = pixelFor((*aliases)[i].get_ref<const std::string&>(),
                                                        key, candidates_)) {
                            const std::string coarse =
                                replaceAll(templateKey, kPixelVariable, *pixel);
                            data(coarse, resolvedTemplate(templateKey, *pixel));
                            return Resolution{
                                coarse, "t:" + templateKey,
                                static_cast<std::uint8_t>(std::min<std::size_t>(i + 1, 255)),
                                *pixel};
                        }
                    }
                }
            }
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                const auto& [templateKey, channel] = *it;
                if (!channel->is_object()) {
                    continue;
                }
                if (const auto pixel = pixelFor(templateKey, key, candidates_)) {
                    data(key, resolvedTemplate(templateKey, *pixel));
                    return Resolution{key, "t:" + templateKey, 0, *pixel};
                }
            }
        }
        return std::nullopt;
    }

    /// The pixel a key would be resolved from as a template channel, if any template gives it.
    std::optional<std::string> templatePixelOf(const std::string& key) {
        if (templates_ == nullptr || candidates_.empty()) {
            return std::nullopt;
        }
        for (const auto& [templateKey, channel] : templates_->items()) {
            if (const auto pixel = pixelFor(templateKey, key, candidates_)) {
                return pixel;
            }
        }
        return std::nullopt;
    }

    /// One mode's entry for `key`, resolved as OFL's `Mode` resolves it — a switching alias first.
    std::string entryFor(const std::string& key, std::string cell, Entry& out) {
        // 1. A switching alias: what the dependency channel's default value selects.
        std::string dependency;
        const json* dependencyObject = nullptr;
        if (const auto found = switchingAliases_.find(key); found != switchingAliases_.end()) {
            dependency = found->second;
            dependencyObject = &available_->at(dependency);
        } else if (templates_ != nullptr && !candidates_.empty()) {
            for (const auto& [templateKey, channel] : templates_->items()) {
                if (!channel.is_object()) {
                    continue;
                }
                for (const std::string& pattern : switchingAliasesOf(channel)) {
                    if (const auto pixel = pixelFor(pattern, key, candidates_)) {
                        dependency = replaceAll(templateKey, kPixelVariable, *pixel);
                        dependencyObject = &resolvedTemplate(templateKey, *pixel);
                        if (cell.empty()) {
                            cell = *pixel;
                        }
                        break;
                    }
                }
                if (dependencyObject != nullptr) {
                    break;
                }
            }
        }
        if (dependencyObject != nullptr) {
            const ChannelData& trigger = data(dependency, *dependencyObject);
            const std::uint64_t value = ofl::scaleValue(
                trigger.defaultRaw.value_or(0), trigger.valueResolution, trigger.maxResolution);
            const json* target = nullptr;
            bool chosen = false;
            for (const Capability& capability : trigger.capabilities) {
                const auto [from, to] = ofl::scaleRange(
                    capability.from, capability.to, trigger.valueResolution, trigger.maxResolution);
                if (capability.hasRange && from <= value && value <= to) {
                    chosen = true;
                    if (capability.switchChannels != nullptr) {
                        target = member(*capability.switchChannels, key.c_str());
                    }
                    break;
                }
            }
            out.cell = std::move(cell);
            const std::string by = "changes with " + quote(dependency);
            if (!chosen || target == nullptr || !(target->is_null() || target->is_string())) {
                out.nothing = true;
                out.label = text::clean(key + " (no default)");
                out.notes.push_back(by + ", and is nothing at that channel's default");
                return {};
            }
            if (target->is_null()) {
                out.nothing = true;
                out.label = text::clean(key + " (off by default)");
                out.notes.push_back(by + ", and is off at that channel's default");
                return {};
            }
            const std::string switched = target->get<std::string>();
            const std::optional<Resolution> resolution = resolveKey(switched);
            if (!resolution) {
                return "channel " + quote(key) + " turns into " + quote(switched) +
                       ", which the file doesn't define";
            }
            fill(out, *resolution, switched);
            out.notes.push_back(by + " — shown as it is at that channel's default");
            return {};
        }
        // 2–4. An available channel, a fine alias, a resolved template channel.
        const std::optional<Resolution> resolution = resolveKey(key);
        if (!resolution) {
            return "it uses a channel the file doesn't define (" + quote(key) + ")";
        }
        out.cell = std::move(cell);
        fill(out, *resolution, key);
        return {};
    }

    void fill(Entry& out, const Resolution& resolution, const std::string& label) {
        out.channel = resolution.channel;
        out.identity = resolution.identity;
        out.byte = resolution.byte;
        out.label = text::clean(label);
        if (out.cell.empty() && !resolution.pixel.empty() &&
            !allGroups_.contains(resolution.pixel)) {
            out.cell = text::clean(resolution.pixel);
        }
    }

    static std::string quote(std::string_view key) { return "'" + text::clean(key) + "'"; }

    /// The mode's channel list, every insert block expanded and every key resolved.
    std::string listChannels(const json& mode, std::vector<Entry>& out) {
        const json* channels = member(mode, "channels");
        if (channels == nullptr || !channels->is_array()) {
            return "its channel list can't be read";
        }
        for (const json& item : *channels) {
            if (out.size() > kChannelLimit) {
                return "it needs more than a universe's 512 channels";
            }
            if (item.is_null()) {
                Entry entry;
                entry.nothing = true;
                entry.label = "(nothing)";
                out.push_back(std::move(entry));
                continue;
            }
            if (item.is_string()) {
                Entry entry;
                if (std::string problem = entryFor(item.get<std::string>(), {}, entry);
                    !problem.empty()) {
                    return problem;
                }
                out.push_back(std::move(entry));
                continue;
            }
            if (!item.is_object() || stringOf(item, "insert") != "matrixChannels") {
                return "its channel list holds something that isn't a channel";
            }
            std::vector<std::string> pixels;
            const json* repeat = member(item, "repeatFor");
            if (repeat == nullptr) {
                return "its per-pixel channels don't say which pixels they repeat for";
            }
            if (std::string problem = repeatFor(*repeat, pixels); !problem.empty()) {
                return problem;
            }
            const json* templateList = member(item, "templateChannels");
            if (templateList == nullptr || !templateList->is_array()) {
                return "its per-pixel channels list no channels";
            }
            const std::string order = stringOf(item, "channelOrder");
            if (order != "perPixel" && order != "perChannel") {
                return "its per-pixel channels don't say what order they come in";
            }
            if (pixels.size() * templateList->size() + out.size() > kChannelLimit) {
                return "it needs " +
                       std::to_string(pixels.size() * templateList->size() + out.size()) +
                       " channels, more than a universe's 512";
            }
            const auto add = [&](const json& templateKey, const std::string& pixel) -> std::string {
                Entry entry;
                if (templateKey.is_null()) {
                    entry.nothing = true;
                    entry.label = "(nothing)";
                    out.push_back(std::move(entry));
                    return {};
                }
                if (!templateKey.is_string()) {
                    return "its per-pixel channels list something that isn't a channel";
                }
                const std::string key =
                    replaceAll(templateKey.get_ref<const std::string&>(), kPixelVariable, pixel);
                if (std::string problem = entryFor(
                        key, allGroups_.contains(pixel) ? std::string() : text::clean(pixel),
                        entry);
                    !problem.empty()) {
                    return problem;
                }
                out.push_back(std::move(entry));
                return {};
            };
            if (order == "perPixel") {
                for (const std::string& pixel : pixels) {
                    for (const json& templateKey : *templateList) {
                        if (std::string problem = add(templateKey, pixel); !problem.empty()) {
                            return problem;
                        }
                    }
                }
            } else {
                for (const json& templateKey : *templateList) {
                    for (const std::string& pixel : pixels) {
                        if (std::string problem = add(templateKey, pixel); !problem.empty()) {
                            return problem;
                        }
                    }
                }
            }
        }
        return {};
    }

    // --- what each channel is --------------------------------------------------------

    std::string wheelTypeOf(const Capability& capability) const {
        if (capability.wheels.empty()) {
            return {};
        }
        const auto found = wheelTypes_.find(capability.wheels.front());
        return found == wheelTypes_.end() ? std::string() : found->second;
    }

    KindReading kindOf(const ChannelData& channel) {
        if (const auto found = kinds_.find(channel.key); found != kinds_.end()) {
            return found->second;
        }
        KindReading reading;
        struct Drive {
            Kind kind;
            std::string word;
        };
        std::vector<Drive> drivable;
        std::vector<std::string> others;
        // One entry per *kind*: lime and indigo on one channel are one kind (another emitter),
        // which is one thing the channel does, not several.
        const auto addDrive = [&](Kind kind, std::string word) {
            const bool known = std::any_of(drivable.begin(), drivable.end(),
                                           [&](const Drive& d) { return d.kind == kind; });
            if (!known) {
                drivable.push_back(Drive{kind, std::move(word)});
            }
        };
        for (const Capability& capability : channel.capabilities) {
            const std::string& type = capability.type;
            if (type == "NoFunction") {
                continue;
            }
            if (type == "Intensity") {
                addDrive(Kind::Dimmer, "dimmer");
            } else if (type == "ColorIntensity") {
                static const std::map<std::string, Kind, std::less<>> kColors{
                    {"Red", Kind::Red},
                    {"Green", Kind::Green},
                    {"Blue", Kind::Blue},
                    {"White", Kind::White},
                    {"Warm White", Kind::WarmWhite},
                    {"Cold White", Kind::CoolWhite},
                    {"Amber", Kind::Amber},
                    {"UV", Kind::Uv},
                    {"Cyan", Kind::CyanSub},
                    {"Magenta", Kind::MagentaSub},
                    {"Yellow", Kind::YellowSub},
                    {"Lime", Kind::OtherEmitter},
                    {"Indigo", Kind::OtherEmitter}};
                const auto color = kColors.find(capability.color);
                addDrive(color == kColors.end() ? Kind::OtherEmitter : color->second,
                         text::lowerAscii(capability.color.empty() ? std::string("color")
                                                                   : capability.color));
            } else if (type == "Pan") {
                addDrive(Kind::Pan, "pan");
            } else if (type == "Tilt") {
                addDrive(Kind::Tilt, "tilt");
            } else if (type == "Zoom") {
                addDrive(Kind::Zoom, "zoom");
            } else if (type == "Focus") {
                addDrive(Kind::Focus, "focus");
            } else if (type == "PanTiltSpeed") {
                addDrive(Kind::PanTiltSpeed, "pan/tilt speed");
            } else if (std::find(others.begin(), others.end(), type) == others.end()) {
                others.push_back(type);
            }
        }
        const auto onlyOf = [&](std::initializer_list<std::string_view> allowed) {
            return std::all_of(others.begin(), others.end(), [&](const std::string& type) {
                return std::find(allowed.begin(), allowed.end(), type) != allowed.end();
            });
        };
        const auto has = [&](std::string_view type) {
            return std::find(others.begin(), others.end(), type) != others.end();
        };
        if (drivable.size() == 1) {
            reading.kind = drivable.front().kind;
            if (!others.empty()) {
                reading.shared = true;
                reading.sharedWith = text::lowerAscii(text::splitCamelCase(others.front()));
            }
        } else if (drivable.size() > 1) {
            reading.kind = Kind::Other;
            std::vector<std::string> words;
            for (const Drive& drive : drivable) {
                words.push_back(drive.word);
            }
            reading.severalThings = "does several things (" + text::join(words, ", ") + ")";
        } else if (has("ShutterStrobe")) {
            reading.kind = Kind::Shutter;
        } else if (!others.empty() && onlyOf({"StrobeSpeed", "StrobeDuration"})) {
            reading.kind = Kind::StrobeRate;
        } else if (has("WheelSlot") &&
                   onlyOf({"WheelSlot", "WheelShake", "WheelRotation", "WheelSlotRotation"})) {
            const auto first =
                std::find_if(channel.capabilities.begin(), channel.capabilities.end(),
                             [](const Capability& c) { return c.type == "WheelSlot"; });
            const std::string wheel = wheelTypeOf(*first);
            reading.kind = wheel == "Color"  ? Kind::ColorWheel
                           : wheel == "Gobo" ? Kind::Gobo
                                             : Kind::Other;
        } else {
            reading.kind = Kind::Other;
        }
        return kinds_.emplace(channel.key, reading).first->second;
    }

    /// The open value of a shutter at `bytes`: the first "Open" shutter capability's menu
    /// value, else a first capability of no function's start.
    static std::optional<std::uint64_t> openValueOf(const ChannelData& channel, unsigned bytes) {
        for (const Capability& capability : channel.capabilities) {
            if (capability.type != "ShutterStrobe" || capability.shutterEffect != "Open" ||
                !capability.hasRange) {
                continue;
            }
            const auto [from, to] =
                ofl::scaleRange(capability.from, capability.to, channel.valueResolution, bytes);
            if (capability.menuClick == "center") {
                return (from + to) / 2;
            }
            if (capability.menuClick == "end") {
                return to;
            }
            return from; // "start", the default, and "hidden"
        }
        if (!channel.capabilities.empty() && channel.capabilities.front().type == "NoFunction" &&
            channel.capabilities.front().hasRange) {
            const Capability& first = channel.capabilities.front();
            return ofl::scaleRange(first.from, first.to, channel.valueResolution, bytes).first;
        }
        return std::nullopt;
    }

    std::vector<DefChannel> assemble(const std::vector<Entry>& entries) {
        // Each channel's resolution in this mode: how many of its own bytes the mode lists.
        std::map<std::string, std::set<std::uint8_t>> bytesOf;
        for (const Entry& entry : entries) {
            if (!entry.nothing) {
                bytesOf[entry.channel].insert(entry.byte);
            }
        }
        std::vector<KindReading> readings(entries.size());
        bool cyan = false;
        bool magenta = false;
        bool yellow = false;
        bool rgb = false;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].nothing) {
                continue;
            }
            readings[i] = kindOf(channels_.at(entries[i].channel));
            const Kind kind = readings[i].kind;
            cyan = cyan || kind == Kind::CyanSub;
            magenta = magenta || kind == Kind::MagentaSub;
            yellow = yellow || kind == Kind::YellowSub;
            rgb = rgb || kind == Kind::Red || kind == Kind::Green || kind == Kind::Blue;
        }
        // A cyan flag and a cyan LED are written alike. All three and no red, green or blue
        // is a CMY head; anything else is an LED that takt4 has no role for.
        const bool subtractive = cyan && magenta && yellow && !rgb;
        std::map<Kind, std::vector<std::string>> ordinals;

        std::vector<DefChannel> out;
        out.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& entry = entries[i];
            DefChannel channel;
            channel.label = entry.label;
            channel.cell = entry.cell;
            channel.notes = entry.notes;
            if (entry.nothing) {
                channel.kind = Kind::Nothing;
                out.push_back(std::move(channel));
                continue;
            }
            const ChannelData& data = channels_.at(entry.channel);
            KindReading reading = readings[i];
            if (!subtractive &&
                (reading.kind == Kind::CyanSub || reading.kind == Kind::MagentaSub ||
                 reading.kind == Kind::YellowSub)) {
                reading.kind = Kind::OtherEmitter;
            }
            channel.kind = reading.kind;
            channel.byte = entry.byte;
            channel.shared = reading.shared;
            channel.sharedWith = reading.sharedWith;
            if (!reading.severalThings.empty()) {
                channel.notes.push_back(reading.severalThings);
            }
            if (reading.kind == Kind::ColorWheel || reading.kind == Kind::Gobo ||
                reading.kind == Kind::Zoom || reading.kind == Kind::Focus) {
                std::vector<std::string>& seen = ordinals[reading.kind];
                auto at = std::find(seen.begin(), seen.end(), entry.identity);
                if (at == seen.end()) {
                    seen.push_back(entry.identity);
                    at = seen.end() - 1;
                }
                channel.ordinal = static_cast<std::uint32_t>(at - seen.begin()) + 1;
            }
            const std::set<std::uint8_t>& present = bytesOf[entry.channel];
            const unsigned resolution = std::clamp<unsigned>(
                std::max<unsigned>(static_cast<unsigned>(present.size()),
                                   static_cast<unsigned>(*present.rbegin()) + 1),
                1, 8);
            channel.resolutionBytes = static_cast<std::uint8_t>(resolution);
            if (data.defaultRaw) {
                channel.defaultValue =
                    ofl::scaleValue(*data.defaultRaw, data.valueResolution, resolution);
            }
            if (data.defaultClamped) {
                channel.notes.push_back("the file's default is too large for it — read as its "
                                        "largest");
            }
            channel.fallbackValue = 0;
            if (reading.kind == Kind::Shutter) {
                channel.openValue = openValueOf(data, resolution);
            }
            for (const Capability& capability : data.capabilities) {
                if (!capability.helpWanted.empty()) {
                    channel.notes.push_back("the library says this channel needs checking: " +
                                            text::clean(capability.helpWanted, 200));
                    break;
                }
            }
            out.push_back(std::move(channel));
        }
        return out;
    }

    const json& root_;
    const json* available_ = nullptr;
    const json* templates_ = nullptr;
    std::map<std::string, std::string> wheelTypes_;
    bool hasMatrix_ = false;
    std::string matrixProblem_;
    std::vector<std::string> insertionOrder_;
    std::unordered_map<std::string, std::array<std::size_t, 3>> positions_;
    std::vector<std::string> pixelsAbc_;
    std::vector<std::string> groupKeys_;
    std::unordered_set<std::string> allGroups_;
    std::unordered_set<std::string> candidates_;
    std::unordered_map<std::string, std::pair<std::string, std::uint8_t>> fineAliases_;
    std::unordered_map<std::string, std::string> switchingAliases_;
    std::unordered_map<std::string, ChannelData> channels_;
    std::unordered_map<std::string, KindReading> kinds_;
    std::deque<json> resolvedStore_;
    std::unordered_map<std::string, const json*> resolved_;
};

/// nlohmann's message for a parse failure, without the library's "[json.exception…] parse
/// error at " in front of it.
std::string describeParseError(const std::exception& error) {
    std::string message = error.what();
    for (const std::string_view drop :
         {std::string_view("] "), std::string_view("parse error at ")}) {
        const std::size_t at = message.find(drop);
        if (at != std::string::npos) {
            message.erase(0, at + drop.size());
        }
    }
    return text::clean(message, 200);
}

} // namespace

ReadResult readOflText(std::string_view text, std::string_view fileName, std::string_view folder,
                       std::string_view stem) {
    ReadResult result;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    if (text::trim(text).empty()) {
        result.problem = "it is empty";
        return result;
    }
    if (text.size() > kOflFileLimit) {
        result.problem = "it is too big to be a fixture file";
        return result;
    }
    if (nestsTooDeep(text)) {
        result.problem = "it isn't a fixture file (it nests far too deep)";
        return result;
    }
    // The order of the pixel groups' keys, which a JSON object does not keep and `eachPixelGroup`
    // needs: recorded as the parser meets them.
    std::vector<std::string> groupOrder;
    struct Frame {
        bool object = false;
        std::string key;
    };
    std::vector<Frame> frames;
    const json::parser_callback_t recorder = [&](int /*depth*/, json::parse_event_t event,
                                                 json& parsed) {
        switch (event) {
        case json::parse_event_t::object_start:
            frames.push_back(Frame{true, {}});
            break;
        case json::parse_event_t::array_start:
            frames.push_back(Frame{false, {}});
            break;
        case json::parse_event_t::object_end:
        case json::parse_event_t::array_end:
            if (!frames.empty()) {
                frames.pop_back();
            }
            break;
        case json::parse_event_t::key:
            if (!frames.empty() && parsed.is_string()) {
                frames.back().key = parsed.get<std::string>();
                if (frames.size() == 3 && frames[0].key == "matrix" && frames[1].object &&
                    frames[1].key == "pixelGroups" && frames[2].object) {
                    groupOrder.push_back(frames.back().key);
                }
            }
            break;
        case json::parse_event_t::value:
            break;
        }
        return true;
    };
    json root;
    try {
        root = json::parse(text, recorder);
    } catch (const std::exception& error) {
        result.problem = "it is damaged, or not a fixture file (" + describeParseError(error) + ")";
        return result;
    }
    const json* name = member(root, "name");
    const json* modes = member(root, "modes");
    const bool hasChannels =
        member(root, "availableChannels") != nullptr || member(root, "templateChannels") != nullptr;
    if (!root.is_object() || name == nullptr || !name->is_string() || modes == nullptr ||
        !modes->is_array() || !hasChannels) {
        result.problem = "it isn't an Open Fixture Library fixture";
        return result;
    }

    Definition definition;
    definition.format = "ofl";
    definition.fileName = text::clean(fileName, 260);
    std::string manufacturer = stringOf(root, "manufacturerKey");
    std::string fixture = stringOf(root, "fixtureKey");
    if (manufacturer.empty()) {
        manufacturer = std::string(folder);
    }
    if (fixture.empty()) {
        fixture = std::string(stem);
    }
    // The download names no manufacturer, only its key: the manufacturer shown is the key.
    definition.manufacturer = text::clean(manufacturer);
    definition.key = text::clean(manufacturer, 200) + "/" + text::clean(fixture, 200);
    definition.model = text::clean(name->get<std::string>());
    if (const json* meta = member(root, "meta")) {
        definition.revision = text::clean(stringOf(*meta, "lastModifyDate"));
    }
    const std::string schema = stringOf(root, "$schema");
    if (const std::size_t at = schema.find("schema-"); at != std::string::npos) {
        const std::size_t dot = schema.find('.', at);
        const auto major = text::parseInteger(std::string_view(schema).substr(
            at + 7, dot == std::string::npos ? std::string::npos : dot - at - 7));
        if (major && *major != 12) {
            definition.notes.push_back("it is written for version " + std::to_string(*major) +
                                       " of the library's format — read as version 12");
        }
    }
    if (const std::string help = stringOf(root, "helpWanted"); !help.empty()) {
        definition.notes.push_back("the library says this fixture needs checking: " +
                                   text::clean(help, 200));
    }

    Reader reader(root, groupOrder);
    std::map<std::string, int> seen;
    std::size_t number = 0;
    for (const json& mode : *modes) {
        ++number;
        std::string modeName = text::clean(stringOf(mode, "name"));
        if (modeName.empty()) {
            modeName = "mode " + std::to_string(number);
        }
        const int count = ++seen[modeName];
        if (count > 1) {
            std::string unique;
            int suffix = count;
            do {
                unique = modeName + " (" + std::to_string(suffix++) + ")";
            } while (seen.contains(unique));
            seen[unique] = 1;
            modeName = unique;
        }
        definition.modes.push_back(reader.readMode(mode, std::move(modeName)));
    }
    if (definition.modes.empty()) {
        result.problem = "it has no modes";
        return result;
    }
    result.definition = std::move(definition);
    return result;
}

ReadResult readOflFile(const std::filesystem::path& path) {
    ReadResult result;
    std::error_code code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code) {
        result.problem = "it could not be read (" + code.message() + ")";
        return result;
    }
    if (size > kOflFileLimit) {
        result.problem =
            "it is " + std::to_string(size / 1024) + " KB, too big to be a fixture file";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.problem = "it could not be opened";
        return result;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(stream.gcount()) != size) {
        result.problem = "it could not be read";
        return result;
    }
    return readOflText(text, io::pathText(path.filename()),
                       io::pathText(path.parent_path().filename()), io::pathText(path.stem()));
}

} // namespace takt4::fixtures
