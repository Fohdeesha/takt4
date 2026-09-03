#include "core/tracking/state_space.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The blob's numbers are copied straight into native storage, which is only right on a
// little-endian host — the only kind takt4 is built for, as in io/npy_file.cpp.
static_assert(std::endian::native == std::endian::little,
              "the state space blob assumes a little-endian host");

namespace takt4::tracking {

namespace {

constexpr std::string_view kMagic = "TAKT4SSP";
constexpr std::uint32_t kFormatVersion = 1;

/// The header, in the order tools/dump_statespace.py packs it. struct.Struct("<...")
/// adds no padding, so the offsets are just the running widths.
struct Header {
    std::uint32_t formatVersion = 0;
    std::uint32_t fps = 0;
    double minBpm = 0.0;
    double maxBpm = 0.0;
    std::uint32_t numTempi = 0;
    double lambdaBeat = 0.0;
    double lambdaDown = 0.0;
    std::uint32_t minBeatsPerBar = 0;
    std::uint32_t maxBeatsPerBar = 0;
    std::uint32_t observationLambdaBeat = 0;
    std::uint32_t observationLambdaDown = 0;
    std::uint32_t beatStates = 0;
    std::uint32_t beatIntervals = 0;
    std::uint32_t beatTransitions = 0;
    std::uint32_t downStates = 0;
    std::uint32_t downIntervals = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t checksum = 0;
};

constexpr std::size_t kHeaderBytes =
    kMagic.size() + 14 * sizeof(std::uint32_t) + 4 * sizeof(double);
static_assert(kHeaderBytes == 96,
              "the fields of Header, packed the way struct.Struct('<...') does");

/// FNV-1a over the payload, the same walk tools/dump_statespace.py makes.
std::uint32_t fnv1a32(const void* data, std::size_t bytes) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint32_t hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < bytes; ++i) {
        hash = (hash ^ p[i]) * 0x01000193u;
    }
    return hash;
}

/// Walks the header and then the payload sections, in order, checking as it goes that
/// the file is long enough for what it claims to hold.
class Reader {
public:
    Reader(const std::vector<unsigned char>& bytes, std::string name)
        : bytes_(bytes), name_(std::move(name)) {}

    std::uint32_t u32() {
        std::uint32_t value = 0;
        std::memcpy(&value, take(sizeof(value)), sizeof(value));
        return value;
    }

    double f64() {
        double value = 0.0;
        std::memcpy(&value, take(sizeof(value)), sizeof(value));
        return value;
    }

    template <typename T>
    std::vector<T> array(std::size_t count) {
        std::vector<T> out(count);
        if (count > 0) {
            std::memcpy(out.data(), take(count * sizeof(T)), count * sizeof(T));
        }
        return out;
    }

    std::size_t offset() const noexcept { return offset_; }

private:
    const unsigned char* take(std::size_t bytes) {
        if (offset_ + bytes > bytes_.size()) {
            throw std::runtime_error(name_ + ": state space blob is truncated");
        }
        const unsigned char* at = bytes_.data() + offset_;
        offset_ += bytes;
        return at;
    }

    const std::vector<unsigned char>& bytes_;
    std::string name_;
    std::size_t offset_ = 0;
};

void require(bool ok, const std::string& name, const std::string& what) {
    if (!ok) {
        throw std::runtime_error(name + ": " + what);
    }
}

/// Reads one state space's six sections, in the order the blob holds them.
StateSpace::Tables readTables(Reader& reader, std::size_t numStates, std::size_t numIntervals) {
    StateSpace::Tables tables;
    tables.intervals = reader.array<std::uint32_t>(numIntervals);
    tables.firstStates = reader.array<std::uint32_t>(numIntervals);
    tables.lastStates = reader.array<std::uint32_t>(numIntervals);
    tables.stateIntervals = reader.array<std::uint32_t>(numStates);
    tables.statePositions = reader.array<double>(numStates);
    tables.pointers = reader.array<std::uint32_t>(numStates);
    return tables;
}

/// Every row of a transition table has to be a distribution over valid destinations.
void checkRows(std::span<const std::uint32_t> offsets, std::span<const std::uint32_t> destinations,
               std::span<const double> probabilities, std::size_t numIntervals,
               const std::string& name) {
    constexpr double kSumTolerance = 1e-9;
    require(offsets.size() == numIntervals + 1, name, "tempo transition rows are miscounted");
    require(offsets.front() == 0, name, "tempo transition rows do not start at zero");
    require(offsets.back() == destinations.size(), name, "tempo transition rows overrun the table");
    for (std::size_t i = 0; i < numIntervals; ++i) {
        require(offsets[i + 1] >= offsets[i], name, "tempo transition rows are not ascending");
        require(offsets[i + 1] > offsets[i], name,
                "interval " + std::to_string(i) + " has nowhere to go");
        double sum = 0.0;
        for (std::size_t k = offsets[i]; k < offsets[i + 1]; ++k) {
            require(destinations[k] < numIntervals, name,
                    "tempo transition " + std::to_string(k) + " leaves the state space");
            require(probabilities[k] > 0.0 && probabilities[k] <= 1.0, name,
                    "tempo transition " + std::to_string(k) + " has an impossible probability");
            sum += probabilities[k];
        }
        require(std::abs(sum - 1.0) <= kSumTolerance, name,
                "the tempo transitions out of interval " + std::to_string(i) + " sum to " +
                    std::to_string(sum));
    }
}

} // namespace

StateSpace::StateSpace(Tables tables, const std::string& context)
    : intervals_(std::move(tables.intervals)), firstStates_(std::move(tables.firstStates)),
      lastStates_(std::move(tables.lastStates)), stateIntervals_(std::move(tables.stateIntervals)),
      statePositions_(std::move(tables.statePositions)), pointers_(std::move(tables.pointers)) {
    const std::size_t states = stateIntervals_.size();
    require(firstStates_.size() == intervals_.size() && lastStates_.size() == intervals_.size() &&
                statePositions_.size() == states && pointers_.size() == states,
            context, "the state space's tables are different lengths");

    intervalIndex_.assign(states, 0);
    std::size_t covered = 0;
    for (std::size_t i = 0; i < intervals_.size(); ++i) {
        const std::uint32_t length = intervals_[i];
        const std::string which = " interval " + std::to_string(i);
        require(length > 0, context, "the state space has an empty interval");
        require(i == 0 || length > intervals_[i - 1], context,
                "the state space's intervals are not ascending and distinct");
        require(covered + length <= states, context, "the state space's intervals overrun it");
        require(firstStates_[i] == covered, context,
                which + " does not start where the last one ended");
        require(lastStates_[i] == covered + length - 1, context,
                which + " is not as long as it says");
        for (std::size_t s = covered; s < covered + length; ++s) {
            require(stateIntervals_[s] == length, context,
                    "state " + std::to_string(s) + " carries the wrong interval");
            require(pointers_[s] <= 2, context,
                    "state " + std::to_string(s) + " has an unknown observation pointer");
            intervalIndex_[s] = static_cast<std::uint32_t>(i);
        }
        // madmom lays the positions out as linspace(0, 1, length, endpoint=False), which
        // is what makes the first state of every interval the (down-)beat.
        require(statePositions_[covered] == 0.0, context, which + " does not start at position 0");
        require(pointers_[covered] == 2, context, which + " does not start on a beat state");
        covered += length;
    }
    require(covered == states, context,
            "the state space's intervals cover " + std::to_string(covered) + " of " +
                std::to_string(states) + " states");
}

std::span<const std::uint32_t>
StateSpaceModel::tempoDestinations(std::size_t interval) const noexcept {
    const std::size_t from = tempoRowOffsets_[interval];
    return std::span<const std::uint32_t>(tempoDestination_)
        .subspan(from, tempoRowOffsets_[interval + 1] - from);
}

std::span<const double> StateSpaceModel::tempoProbabilities(std::size_t interval) const noexcept {
    const std::size_t from = tempoRowOffsets_[interval];
    return std::span<const double>(tempoProbability_)
        .subspan(from, tempoRowOffsets_[interval + 1] - from);
}

std::span<const double> StateSpaceModel::meterTransitions(std::size_t meter) const noexcept {
    const std::size_t width = downbeat_.numIntervals();
    return std::span<const double>(meterTransitions_).subspan(meter * width, width);
}

StateSpaceModel StateSpaceModel::fromFile(const std::filesystem::path& path) {
    const std::string name = path.string();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(name + ": cannot open");
    }
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
    if (bytes.size() < kHeaderBytes) {
        throw std::runtime_error(name + ": too short to be a takt4 state space blob");
    }
    if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
        throw std::runtime_error(name + ": not a takt4 state space blob");
    }

    Reader reader(bytes, name);
    (void)reader.array<unsigned char>(kMagic.size());
    Header header;
    header.formatVersion = reader.u32();
    if (header.formatVersion != kFormatVersion) {
        throw std::runtime_error(name + ": state space blob format version " +
                                 std::to_string(header.formatVersion) +
                                 ", this build reads version " + std::to_string(kFormatVersion));
    }
    header.fps = reader.u32();
    header.minBpm = reader.f64();
    header.maxBpm = reader.f64();
    header.numTempi = reader.u32();
    header.lambdaBeat = reader.f64();
    header.lambdaDown = reader.f64();
    header.minBeatsPerBar = reader.u32();
    header.maxBeatsPerBar = reader.u32();
    header.observationLambdaBeat = reader.u32();
    header.observationLambdaDown = reader.u32();
    header.beatStates = reader.u32();
    header.beatIntervals = reader.u32();
    header.beatTransitions = reader.u32();
    header.downStates = reader.u32();
    header.downIntervals = reader.u32();
    header.payloadBytes = reader.u32();
    header.checksum = reader.u32();

    require(reader.offset() == kHeaderBytes, name, "the header is not the size this build expects");
    require(bytes.size() == kHeaderBytes + header.payloadBytes, name,
            "state space blob holds " + std::to_string(bytes.size() - kHeaderBytes) +
                " bytes of payload, its header says " + std::to_string(header.payloadBytes));
    const std::uint32_t checksum = fnv1a32(bytes.data() + kHeaderBytes, header.payloadBytes);
    require(checksum == header.checksum, name,
            "state space blob checksum mismatch (file says " + std::to_string(header.checksum) +
                ", contents give " + std::to_string(checksum) + ")");
    require(header.fps > 0, name, "frame rate is zero");
    require(header.beatIntervals > 0 && header.downIntervals > 0, name, "a state space is empty");

    StateSpaceModel model;
    model.path_ = path;
    model.config_ = Config{header.fps,
                           header.minBpm,
                           header.maxBpm,
                           header.numTempi,
                           header.lambdaBeat,
                           header.lambdaDown,
                           header.minBeatsPerBar,
                           header.maxBeatsPerBar,
                           header.observationLambdaBeat,
                           header.observationLambdaDown};

    model.beat_ = StateSpace(readTables(reader, header.beatStates, header.beatIntervals),
                             name + ": the beat");
    model.tempoRowOffsets_ = reader.array<std::uint32_t>(header.beatIntervals + 1);
    model.tempoDestination_ = reader.array<std::uint32_t>(header.beatTransitions);
    model.tempoProbability_ = reader.array<double>(header.beatTransitions);
    model.downbeat_ = StateSpace(readTables(reader, header.downStates, header.downIntervals),
                                 name + ": the downbeat");
    model.meterTransitions_ =
        reader.array<double>(static_cast<std::size_t>(header.downIntervals) * header.downIntervals);

    require(reader.offset() == bytes.size(), name,
            "state space blob is longer than its sections account for");

    checkRows(model.tempoRowOffsets_, model.tempoDestination_, model.tempoProbability_,
              header.beatIntervals, name);
    for (std::size_t i = 0; i < header.downIntervals; ++i) {
        double sum = 0.0;
        for (const double p : model.meterTransitions(i)) {
            require(p > 0.0 && p <= 1.0, name, "a meter transition has an impossible probability");
            sum += p;
        }
        require(std::abs(sum - 1.0) <= 1e-9, name,
                "the meter transitions out of meter " + std::to_string(i) + " sum to " +
                    std::to_string(sum));
    }
    return model;
}

} // namespace takt4::tracking
