#include "core/tracking/particle_filter.hpp"

#include "core/io/npy_file.hpp"
#include "core/rt/alloc_guard.hpp"
#include "core/tracking/random.hpp"
#include "core/tracking/state_space.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Approx;
using takt4::tracking::ParticleFilter;
using takt4::tracking::StateSpaceModel;
using takt4::tracking::TrackedFrame;
using takt4::tracking::Xoshiro256pp;

namespace {

const std::filesystem::path kDataDir{TAKT4_TEST_DATA_DIR};
const std::filesystem::path kBlob = std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";

// The 18 golden excerpts, the same set Phases 2 and 3 are gated on.
const std::vector<std::string> kExcerpts{"big-brown-beaver",
                                         "complicated-geometry",
                                         "good-times",
                                         "hell-of-a-ride",
                                         "in-yer-face",
                                         "jack-yourself",
                                         "keman-rhythm",
                                         "model-2029",
                                         "outside-plume",
                                         "pirates",
                                         "rale",
                                         "synthetic",
                                         "the-galaxist",
                                         "trigger-finger",
                                         "true-believer",
                                         "vic-acid",
                                         "winter-now",
                                         "your-sweet-boom"};

const StateSpaceModel& model() {
    static const StateSpaceModel loaded = StateSpaceModel::fromFile(kBlob);
    return loaded;
}

} // namespace

// xoshiro256++ is only worth specifying if both sides compute it identically. These are
// the first draws for seed 1, taken from tools/pf_reference.py's Xoshiro256pp — if the
// C++ and the Python disagree here, nothing below can mean anything.
TEST_CASE("the generator produces the stream both sides are written against",
          "[tracking][random]") {
    Xoshiro256pp rng(1);
    const std::vector<std::uint64_t> expected{14971601782005023387ull, 13781649495232077965ull,
                                              1847458086238483744ull,  13765271635752736470ull,
                                              3406718355780431780ull,  10892412867582108485ull,
                                              18204613561675945223ull, 9655336933892813345ull};
    for (const std::uint64_t want : expected) {
        CHECK(rng.next() == want);
    }
    Xoshiro256pp doubles(1);
    CHECK(doubles.nextDouble() == Approx(0.811612158882).margin(1e-12));
    CHECK(doubles.nextDouble() == Approx(0.747104716158).margin(1e-12));
    CHECK(doubles.nextDouble() == Approx(0.100150903534).margin(1e-12));

    SECTION("doubles land in [0, 1) and bounded draws inside their bound") {
        Xoshiro256pp other(12345);
        std::vector<std::size_t> seen(7, 0);
        for (int i = 0; i < 20000; ++i) {
            const double value = other.nextDouble();
            REQUIRE(value >= 0.0);
            REQUIRE(value < 1.0);
            const std::uint64_t bounded = other.bounded(7);
            REQUIRE(bounded < 7);
            ++seen[bounded];
        }
        // Not a randomness test — just that the rejection loop is not stuck on one value.
        for (const std::size_t count : seen) {
            CHECK(count > 20000 / 7 / 2);
        }
    }

    SECTION("reseeding replays the same stream") {
        Xoshiro256pp a(99);
        Xoshiro256pp b(7);
        for (int i = 0; i < 100; ++i) {
            (void)a.next();
            (void)b.next();
        }
        b.reseed(99);
        Xoshiro256pp fresh(99);
        for (int i = 0; i < 100; ++i) {
            CHECK(b.next() == fresh.next());
        }
    }
}

// The Phase 4 gate. tools/pf_reference.py restates BeatNet+'s filter with this exact
// generator and writes one row per frame; the C++ has to reproduce every one of them.
// A particle filter has no exact answer to be held to — upstream's own output moves
// when only the seed changes — so the comparison is against the restatement, and
// tests/data/tracking/README.md records what holds the restatement to upstream.
TEST_CASE("the filter reproduces the reference trace frame for frame",
          "[tracking][filter][parity]") {
    std::size_t frames = 0;
    std::size_t beats = 0;
    for (const std::string& name : kExcerpts) {
        INFO("excerpt " << name);
        const takt4::io::NpyMatrix activations =
            takt4::io::readNpyFloat32(kDataDir / "model" / "generic" / (name + ".npy"));
        const takt4::io::NpyInt32Matrix reference =
            takt4::io::readNpyInt32(kDataDir / "tracking" / (name + ".npy"));
        REQUIRE(activations.cols == 6);
        REQUIRE(reference.cols == 4);
        REQUIRE(reference.rows == activations.rows);

        ParticleFilter filter(model());
        for (std::size_t f = 0; f < activations.rows; ++f) {
            // Columns 3 and 4 are the softmax probabilities for beat and downbeat.
            const TrackedFrame got = filter.process(activations.at(f, 3), activations.at(f, 4));
            INFO("frame " << f);
            REQUIRE(static_cast<std::int32_t>(got.gathering) == reference.at(f, 0));
            REQUIRE(static_cast<std::int32_t>(got.downMax) == reference.at(f, 1));
            REQUIRE(static_cast<std::int32_t>(got.emitted) == reference.at(f, 2));
            REQUIRE(static_cast<std::int32_t>(got.intervalFrames) == reference.at(f, 3));
            beats += got.emitted != TrackedFrame::Emitted::None ? 1 : 0;
        }
        frames += activations.rows;
    }
    // If the excerpts or the reference are ever regenerated smaller, the loop above
    // could pass while checking almost nothing.
    CHECK(frames == 18 * 500);
    CHECK(beats > 300);
}

TEST_CASE("the filter is deterministic and resettable", "[tracking][filter]") {
    const takt4::io::NpyMatrix activations =
        takt4::io::readNpyFloat32(kDataDir / "model" / "generic" / "good-times.npy");

    ParticleFilter a(model());
    ParticleFilter b(model());
    for (std::size_t f = 0; f < activations.rows; ++f) {
        const TrackedFrame first = a.process(activations.at(f, 3), activations.at(f, 4));
        const TrackedFrame second = b.process(activations.at(f, 3), activations.at(f, 4));
        REQUIRE(first.gathering == second.gathering);
        REQUIRE(first.emitted == second.emitted);
    }

    // A reset has to put the filter back where it started, or a stream that is stopped
    // and started again is not the same tracker.
    const std::vector<std::uint32_t> after = a.particles();
    a.reset();
    ParticleFilter fresh(model());
    CHECK(a.particles() == fresh.particles());
    CHECK(a.downbeatParticles() == fresh.downbeatParticles());
    CHECK(a.particles() != after);
    for (std::size_t f = 0; f < activations.rows; ++f) {
        const TrackedFrame again = a.process(activations.at(f, 3), activations.at(f, 4));
        const TrackedFrame first = fresh.process(activations.at(f, 3), activations.at(f, 4));
        REQUIRE(again.gathering == first.gathering);
        REQUIRE(again.emitted == first.emitted);
    }

    SECTION("a different seed gives a different run of the same filter") {
        ParticleFilter other(model(), ParticleFilter::Options{1500, 250, 20260903});
        CHECK(other.particles() != fresh.particles());
    }
}

TEST_CASE("both clouds keep their size, whatever the activations do", "[tracking][filter]") {
    // Upstream's injections are never undone — `np.delete` is called without its result
    // being assigned, and with a count of 1 where 7 were added — so its beat cloud grows
    // from 1500 to about 1850 over ten seconds and would reach six figures over a night.
    // Nothing here may grow: see tools/pf_reference.py's header.
    ParticleFilter filter(model());
    Xoshiro256pp noise(4);
    for (int frame = 0; frame < 3000; ++frame) {
        // Deliberately spend most frames above the 0.8 injection threshold.
        const float beat = static_cast<float>(0.6 + 0.4 * noise.nextDouble());
        const float downbeat = static_cast<float>(0.6 + 0.4 * noise.nextDouble());
        (void)filter.process(beat, downbeat);
        REQUIRE(filter.particles().size() == 1500);
        REQUIRE(filter.downbeatParticles().size() == 250);
    }
}

TEST_CASE("the filter reports a tempo, a phase and a meter it could act on", "[tracking][filter]") {
    const takt4::io::NpyMatrix activations =
        takt4::io::readNpyFloat32(kDataDir / "model" / "generic" / "synthetic.npy");
    ParticleFilter filter(model());
    std::vector<std::uint64_t> beatFrames;
    TrackedFrame last;
    for (std::size_t f = 0; f < activations.rows; ++f) {
        last = filter.process(activations.at(f, 3), activations.at(f, 4));
        CHECK(last.bpm >= 54.0);
        CHECK(last.bpm <= 215.0);
        CHECK(last.phase >= 0.0);
        CHECK(last.phase < 1.0);
        CHECK(last.beatsPerBar >= 2);
        CHECK(last.beatsPerBar <= 4);
        CHECK(last.tempoAgreement >= 0.0);
        CHECK(last.tempoAgreement <= 1.0);
        if (last.emitted != TrackedFrame::Emitted::None) {
            beatFrames.push_back(last.frameIndex);
        }
    }
    CHECK(last.frameIndex == activations.rows - 1);

    // synthetic.wav is a drum machine at 128 BPM, which Phase 3 already measured as
    // peaks 23 frames apart. The tracker has to land on the same period.
    REQUIRE(beatFrames.size() > 10);
    std::vector<double> gaps;
    for (std::size_t i = 1; i < beatFrames.size(); ++i) {
        gaps.push_back(static_cast<double>(beatFrames[i] - beatFrames[i - 1]));
    }
    double total = 0.0;
    for (const double gap : gaps) {
        total += gap;
    }
    const double meanGap = total / static_cast<double>(gaps.size());
    CHECK(meanGap == Approx(23.4).margin(1.0));
    CHECK(60.0 / (meanGap * 0.02) == Approx(128.0).margin(6.0));
}

TEST_CASE("a frame of tracking touches no heap", "[tracking][filter][rt]") {
    // §4.2 puts the filter on the inference thread, which may allocate. It does not, and
    // this keeps it that way: the resampling step's cost is data-dependent enough
    // already without a heap in it.
    const takt4::io::NpyMatrix activations =
        takt4::io::readNpyFloat32(kDataDir / "model" / "generic" / "vic-acid.npy");
    ParticleFilter filter(model());
    // Warm up outside the scope: the first frames are no different, but this makes the
    // measurement about process() rather than about anything the constructor deferred.
    for (std::size_t f = 0; f < 10; ++f) {
        (void)filter.process(activations.at(f, 3), activations.at(f, 4));
    }

    const bool abortWas = takt4::rt::abortOnViolation();
    takt4::rt::setAbortOnViolation(false);
    const std::uint64_t before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope scope;
        for (std::size_t f = 10; f < activations.rows; ++f) {
            (void)filter.process(activations.at(f, 3), activations.at(f, 4));
        }
    }
    const std::uint64_t after = takt4::rt::violationCount();
    takt4::rt::setAbortOnViolation(abortWas);
    CHECK(takt4::rt::allocationGuardEnabled());
    CHECK(after == before);
}
