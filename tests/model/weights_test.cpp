#include "core/model/weights.hpp"

#include "core/model/dimensions.hpp"
#include "support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using takt4::model::kLstmLayers;
using takt4::model::kTotalParameters;
using takt4::model::ModelWeights;
using takt4::test::TempDir;

namespace {

const std::filesystem::path kWeightsDir{TAKT4_WEIGHTS_DIR};

std::vector<char> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("every committed weight set loads and is the shape this build expects", "[model][weights]") {
    for (const std::string name : {"generic", "generic-main", "af-non-percussive"}) {
        INFO("weight set " << name);
        const std::filesystem::path path = kWeightsDir / (name + ".bin");
        REQUIRE(std::filesystem::exists(path));

        const ModelWeights weights = ModelWeights::fromFile(path);
        CHECK(weights.convWeight().size() == takt4::model::kConvFilters * takt4::model::kKernelSize);
        CHECK(weights.convBias().size() == takt4::model::kConvFilters);
        CHECK(weights.denseWeight().size() == takt4::model::kHidden * takt4::model::kDenseIn);
        CHECK(weights.denseBias().size() == takt4::model::kHidden);
        CHECK(weights.outputWeight().size() == takt4::model::kNumClasses * takt4::model::kHidden);
        CHECK(weights.outputBias().size() == takt4::model::kNumClasses);

        std::size_t total = weights.convWeight().size() + weights.convBias().size() + weights.denseWeight().size() +
                            weights.denseBias().size() + weights.outputWeight().size() + weights.outputBias().size();
        for (std::size_t layer = 0; layer < kLstmLayers; ++layer) {
            const takt4::model::LstmWeights lstm = weights.lstm(layer);
            CHECK(lstm.weightIh.size() == takt4::model::kGates * takt4::model::kHidden * takt4::model::kHidden);
            CHECK(lstm.weightHh.size() == lstm.weightIh.size());
            CHECK(lstm.biasIh.size() == takt4::model::kGates * takt4::model::kHidden);
            CHECK(lstm.biasHh.size() == lstm.biasIh.size());
            total += lstm.weightIh.size() + lstm.weightHh.size() + lstm.biasIh.size() + lstm.biasHh.size();
        }
        CHECK(total == kTotalParameters);

        // A weight set that is all zeros, or holds a NaN, would load happily; neither is
        // a plausible trained network.
        bool sane = true;
        double magnitude = 0.0;
        for (const float v : weights.denseWeight()) {
            sane = sane && std::isfinite(v);
            magnitude += std::abs(static_cast<double>(v));
        }
        CHECK(sane);
        CHECK(magnitude > 0.0);
    }
}

TEST_CASE("a damaged weight blob is refused rather than loaded", "[model][weights]") {
    const std::vector<char> good = readAll(kWeightsDir / "generic.bin");
    REQUIRE(good.size() > 64);
    const TempDir dir;

    SECTION("wrong magic") {
        std::vector<char> bytes = good;
        bytes[0] = 'X';
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("a format version this build does not know") {
        std::vector<char> bytes = good;
        bytes[8] = 99;
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("a dimension that is not this architecture's") {
        std::vector<char> bytes = good;
        bytes[12] = 0; // feature dimension, first byte of 288
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("truncated") {
        std::vector<char> bytes(good.begin(), good.end() - 4);
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("longer than the header says") {
        std::vector<char> bytes = good;
        bytes.push_back(0);
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("one flipped bit in the parameters") {
        std::vector<char> bytes = good;
        bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 1);
        write(dir.file("bad.bin"), bytes);
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("bad.bin")), std::runtime_error);
    }
    SECTION("missing") {
        CHECK_THROWS_AS(ModelWeights::fromFile(dir.file("absent.bin")), std::runtime_error);
    }
}
