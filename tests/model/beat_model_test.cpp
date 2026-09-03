#include "core/io/npy_file.hpp"
#include "core/model/beat_model.hpp"
#include "core/model/dimensions.hpp"
#include "core/model/weights.hpp"
#include "core/rt/alloc_guard.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::model::BeatModel;
using takt4::model::kFeatureDim;
using takt4::model::kNumClasses;
using takt4::model::ModelWeights;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};
const std::filesystem::path kWeightsDir{TAKT4_WEIGHTS_DIR};

// The reference traces are (frames, 6): the logits, then the softmax of them.
constexpr std::size_t kReferenceColumns = 2 * kNumClasses;

using Frame = std::array<float, kFeatureDim>;

std::vector<Frame> featureFrames(const std::filesystem::path& path) {
    const takt4::io::NpyMatrix golden = takt4::io::readNpyFloat32(path);
    REQUIRE(golden.cols == kFeatureDim);
    std::vector<Frame> frames(golden.rows);
    for (std::size_t f = 0; f < golden.rows; ++f) {
        std::copy_n(golden.values.begin() + static_cast<std::ptrdiff_t>(f * kFeatureDim),
                    kFeatureDim, frames[f].begin());
    }
    return frames;
}

std::vector<std::string> weightSets() {
    return {"generic", "generic-main", "af-non-percussive"};
}

std::vector<std::filesystem::path> referenceTraces(const std::string& set) {
    std::vector<std::filesystem::path> traces;
    const std::filesystem::path dir = kTestData / "model" / set;
    if (!std::filesystem::is_directory(dir)) {
        return traces;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".npy") {
            traces.push_back(entry.path());
        }
    }
    std::sort(traces.begin(), traces.end());
    return traces;
}

} // namespace

// HANDOFF §8 Phase 3, the gate: "activation traces match PyTorch to tolerance on the
// Phase 2 test set". The traces under tests/data/model/ are what the real BeatNet+
// makes of the committed madmom features of every golden excerpt, recorded once by
// tools/model_reference.py; the input here is those same madmom features, so a Phase 2
// regression shows up in the feature test rather than muddying this one.
TEST_CASE("the C++ model matches PyTorch on every golden excerpt", "[model][golden]") {
    // Measured first, then set above what was measured, the way the feature tolerance
    // was. On MSVC 2022 x64, Release, across all 54 excerpt-and-weight-set pairs: the
    // largest logit difference is 1.29e-5 and the largest probability difference
    // 3.58e-6. For scale, PyTorch's own frame-by-frame and whole-sequence runs of the
    // same weights differ by up to 7.3e-6 (tests/data/model/*.json), so this is within
    // a factor of two of what float32 reordering costs PyTorch itself, and there is
    // nothing left to chase below it. The margin is for the other two compilers.
    constexpr double kLogitTolerance = 1e-4;
    constexpr double kProbabilityTolerance = 2e-5;
    constexpr std::size_t kRequiredExcerpts = 10;

    for (const std::string& set : weightSets()) {
        INFO("weight set " << set);
        const std::vector<std::filesystem::path> traces = referenceTraces(set);
        REQUIRE(traces.size() >= kRequiredExcerpts);

        const ModelWeights weights = ModelWeights::fromFile(kWeightsDir / (set + ".bin"));
        BeatModel model(weights);

        double worstLogit = 0.0;
        double worstProbability = 0.0;
        std::string worstWhere;
        for (const std::filesystem::path& trace : traces) {
            INFO("excerpt " << trace.filename().string());
            const takt4::io::NpyMatrix reference = takt4::io::readNpyFloat32(trace);
            REQUIRE(reference.cols == kReferenceColumns);

            const std::vector<Frame> frames =
                featureFrames(kTestData / "features" / trace.filename());
            REQUIRE(frames.size() == reference.rows);

            model.reset();
            for (std::size_t f = 0; f < frames.size(); ++f) {
                const BeatModel::Activation activation = model.process(frames[f]);
                for (std::size_t c = 0; c < kNumClasses; ++c) {
                    const double logit = std::abs(static_cast<double>(activation.logits[c]) -
                                                  static_cast<double>(reference.at(f, c)));
                    const double probability =
                        std::abs(static_cast<double>(activation.probabilities[c]) -
                                 static_cast<double>(reference.at(f, kNumClasses + c)));
                    if (logit > worstLogit) {
                        worstLogit = logit;
                        worstWhere = trace.filename().string() + " frame " + std::to_string(f) +
                                     " class " + std::to_string(c);
                    }
                    worstProbability = std::max(worstProbability, probability);
                }
            }
            REQUIRE(model.framesProcessed() == frames.size());
        }
        INFO("largest logit difference " << worstLogit << " at " << worstWhere
                                         << "; largest probability difference "
                                         << worstProbability);
        CHECK(worstLogit <= kLogitTolerance);
        CHECK(worstProbability <= kProbabilityTolerance);
    }
}

TEST_CASE("the model's probabilities are a distribution over the three classes", "[model]") {
    const ModelWeights weights = ModelWeights::fromFile(kWeightsDir / "generic.bin");
    BeatModel model(weights);
    const std::vector<Frame> frames = featureFrames(kTestData / "features" / "synthetic.npy");
    REQUIRE(frames.size() > 100);

    double loudest = 0.0;
    for (const Frame& frame : frames) {
        const BeatModel::Activation activation = model.process(frame);
        double total = 0.0;
        for (const float p : activation.probabilities) {
            CHECK(p >= 0.0f);
            CHECK(p <= 1.0f);
            total += static_cast<double>(p);
        }
        CHECK_THAT(total, WithinAbs(1.0, 1e-6));
        loudest = std::max(loudest, static_cast<double>(activation.beat()));
    }
    // The synthetic excerpt is a drum machine at 128 BPM; a working model finds beats
    // in it. Anything much below this and the weights or the wiring are wrong.
    CHECK(loudest > 0.5);
}

TEST_CASE("reset() puts the model back where it started", "[model]") {
    const ModelWeights weights = ModelWeights::fromFile(kWeightsDir / "generic.bin");
    BeatModel model(weights);
    const std::vector<Frame> frames = featureFrames(kTestData / "features" / "synthetic.npy");
    REQUIRE(frames.size() > 20);

    std::vector<BeatModel::Activation> first;
    for (std::size_t f = 0; f < 20; ++f) {
        first.push_back(model.process(frames[f]));
    }
    CHECK(model.framesProcessed() == 20);

    // Run on for a while so the LSTM state is anything but fresh, then reset.
    for (std::size_t f = 20; f < frames.size(); ++f) {
        (void)model.process(frames[f]);
    }
    model.reset();
    CHECK(model.framesProcessed() == 0);

    for (std::size_t f = 0; f < first.size(); ++f) {
        INFO("frame " << f);
        const BeatModel::Activation again = model.process(frames[f]);
        for (std::size_t c = 0; c < kNumClasses; ++c) {
            CHECK(again.logits[c] == first[f].logits[c]);
        }
    }
}

TEST_CASE("silence keeps the model quiet and finite", "[model]") {
    const ModelWeights weights = ModelWeights::fromFile(kWeightsDir / "generic.bin");
    BeatModel model(weights);
    const Frame silence{};
    for (int f = 0; f < 200; ++f) {
        const BeatModel::Activation activation = model.process(silence);
        for (const float p : activation.probabilities) {
            REQUIRE(std::isfinite(p));
        }
        CHECK(activation.nonBeat() > activation.beat());
    }
}

TEST_CASE("BeatModel::process() does not touch the heap", "[model][rt]") {
    REQUIRE(takt4::rt::allocationGuardEnabled());
    const ModelWeights weights = ModelWeights::fromFile(kWeightsDir / "generic.bin");
    BeatModel model(weights);
    const std::vector<Frame> frames = featureFrames(kTestData / "features" / "synthetic.npy");
    REQUIRE(frames.size() > 50);
    float sink = 0.0f;

    takt4::rt::setAbortOnViolation(false);
    const auto before = takt4::rt::violationCount();
    {
        const takt4::rt::RealtimeScope realtime;
        for (std::size_t f = 0; f < 50; ++f) {
            sink += model.process(frames[f]).beat();
        }
        model.reset();
    }
    takt4::rt::setAbortOnViolation(true);
    CHECK(takt4::rt::violationCount() == before);
    CHECK(sink >= 0.0f);
}
