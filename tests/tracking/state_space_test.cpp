#include "core/tracking/state_space.hpp"

#include "support/temp_dir.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <vector>

using Catch::Approx;
using takt4::test::TempDir;
using takt4::tracking::StateSpace;
using takt4::tracking::StateSpaceModel;

namespace {

const std::filesystem::path kBlob = std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";

std::vector<char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

void write(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// The numbers here were read out of madmom in the venv, not out of HANDOFF §5.4: at
// 55-215 BPM and 50 fps the beat state space is 42 integer intervals of 14 to 55 frames
// laid end to end into 1449 states. If a rebuilt blob disagrees, either madmom changed
// or tools/dump_statespace.py was run with different arguments, and the tracker that
// was tuned against the old one is not the tracker this would give.
TEST_CASE("the committed state space blob is the one BeatNet+ is configured for",
          "[tracking][statespace]") {
    REQUIRE(std::filesystem::exists(kBlob));
    const StateSpaceModel model = StateSpaceModel::fromFile(kBlob);

    const auto& config = model.config();
    CHECK(config.fps == 50);
    CHECK(config.minBpm == 55.0);
    CHECK(config.maxBpm == 215.0);
    CHECK(config.numTempi == 300);
    CHECK(config.lambdaBeat == 60.0);
    CHECK(config.lambdaDown == 0.1);
    CHECK(config.minBeatsPerBar == 2);
    CHECK(config.maxBeatsPerBar == 4);
    CHECK(config.observationLambdaBeat == 56);
    CHECK(config.observationLambdaDown == 56);
    CHECK(model.secondsPerFrame() == 0.02);

    const StateSpace& beat = model.beat();
    CHECK(beat.numStates() == 1449);
    CHECK(beat.numIntervals() == 42);
    CHECK(beat.intervals().front() == 14);
    CHECK(beat.intervals().back() == 55);
    // madmom's linear spacing: every whole interval in the range, none missing.
    for (std::size_t i = 0; i < beat.numIntervals(); ++i) {
        CHECK(beat.intervals()[i] == 14 + i);
    }
    CHECK(std::accumulate(beat.intervals().begin(), beat.intervals().end(), std::size_t{0}) ==
          beat.numStates());
    CHECK(model.bpmOfInterval(0) == Approx(214.2857).epsilon(1e-6));
    CHECK(model.bpmOfInterval(beat.numIntervals() - 1) == Approx(54.5455).epsilon(1e-6));

    const StateSpace& down = model.downbeat();
    CHECK(down.numStates() == 9);
    CHECK(down.numIntervals() == 3);
    CHECK(down.intervals()[0] == 2);
    CHECK(down.intervals()[1] == 3);
    CHECK(down.intervals()[2] == 4);
    CHECK(down.firstStates()[2] == 5);
    CHECK(down.lastStates()[2] == 8);
}

TEST_CASE("a state knows its interval, its phase and whether it is a beat",
          "[tracking][statespace]") {
    const StateSpaceModel model = StateSpaceModel::fromFile(kBlob);
    const StateSpace& beat = model.beat();

    std::size_t beatStates = 0;
    for (std::size_t s = 0; s < beat.numStates(); ++s) {
        const std::size_t interval = beat.intervalOf(s);
        REQUIRE(interval < beat.numIntervals());
        CHECK(beat.stateIntervals()[s] == beat.intervals()[interval]);
        CHECK(beat.phaseOf(s) < beat.intervals()[interval]);
        CHECK(beat.firstStates()[interval] + beat.phaseOf(s) == s);
        // madmom's linspace(0, 1, interval, endpoint=False).
        CHECK(beat.statePositions()[s] == Approx(static_cast<double>(beat.phaseOf(s)) /
                                                 static_cast<double>(beat.intervals()[interval]))
                                              .margin(1e-12));
        // The one thing the filter asks of the observation model: a state is a beat
        // state exactly when it is the first of its interval.
        CHECK(beat.isBeatState(s) == (beat.phaseOf(s) == 0));
        if (beat.isBeatState(s)) {
            ++beatStates;
        }
    }
    CHECK(beatStates == beat.numIntervals());

    // The downbeat space's pointers are the meter's: the first state of each meter is a
    // downbeat state, the rest are ordinary beats.
    const StateSpace& down = model.downbeat();
    for (std::size_t s = 0; s < down.numStates(); ++s) {
        CHECK(down.isBeatState(s) == (down.phaseOf(s) == 0));
    }
}

TEST_CASE("the tempo transitions are an exponential distribution around the same tempo",
          "[tracking][statespace]") {
    const StateSpaceModel model = StateSpaceModel::fromFile(kBlob);
    const StateSpace& beat = model.beat();

    std::size_t stored = 0;
    for (std::size_t i = 0; i < beat.numIntervals(); ++i) {
        const auto destinations = model.tempoDestinations(i);
        const auto probabilities = model.tempoProbabilities(i);
        REQUIRE(destinations.size() == probabilities.size());
        REQUIRE(!destinations.empty());
        stored += destinations.size();

        double sum = 0.0;
        double best = 0.0;
        std::size_t likeliest = destinations.size();
        for (std::size_t k = 0; k < destinations.size(); ++k) {
            sum += probabilities[k];
            if (probabilities[k] > best) {
                best = probabilities[k];
                likeliest = destinations[k];
            }
        }
        CHECK(sum == Approx(1.0).margin(1e-12));
        // exp(-lambda * |ratio - 1|) peaks where the ratio is 1, so keeping the tempo is
        // always the likeliest move. Get the row indexing wrong and this stops holding.
        CHECK(likeliest == i);
        // Destinations are ascending, as madmom's dense model lists them.
        for (std::size_t k = 1; k < destinations.size(); ++k) {
            CHECK(destinations[k] > destinations[k - 1]);
        }
    }
    // 1315 of madmom's 2722 transitions; the other 1407 are the "+1 within the interval"
    // ones, which carry no information. assets/statespace/default.json records both.
    CHECK(stored == 1315);

    // The meter transitions are BeatNet+'s own: 1 - lambda to stay, the rest shared out.
    for (std::size_t i = 0; i < model.downbeat().numIntervals(); ++i) {
        const auto row = model.meterTransitions(i);
        REQUIRE(row.size() == 3);
        for (std::size_t j = 0; j < row.size(); ++j) {
            CHECK(row[j] == Approx(i == j ? 0.9 : 0.05).margin(1e-12));
        }
    }
}

TEST_CASE("a damaged state space blob is refused rather than loaded", "[tracking][statespace]") {
    const std::vector<char> good = readAll(kBlob);
    REQUIRE(good.size() > 128);
    const TempDir dir;

    SECTION("wrong magic") {
        std::vector<char> bytes = good;
        bytes[0] = 'X';
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(StateSpaceModel::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("a format version this build does not know") {
        std::vector<char> bytes = good;
        bytes[8] = 99;
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(StateSpaceModel::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("truncated") {
        std::vector<char> bytes(good.begin(), good.end() - 8);
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(StateSpaceModel::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("longer than the header says") {
        std::vector<char> bytes = good;
        bytes.push_back(0);
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(StateSpaceModel::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("one flipped bit in the tables") {
        std::vector<char> bytes = good;
        const std::size_t at = bytes.size() / 2;
        bytes[at] = static_cast<char>(bytes[at] ^ 1);
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(StateSpaceModel::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("missing") {
        CHECK_THROWS_AS(StateSpaceModel::fromFile(dir.file("absent.bin")), std::runtime_error);
    }
}
