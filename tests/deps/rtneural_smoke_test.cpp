// RTNeural smoke test. This TU must not include <nlohmann/json.hpp>: RTNeural bundles
// an older copy; see cmake/deps.cmake.

#include <RTNeural/RTNeural.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <vector>

using Catch::Matchers::WithinAbs;

TEST_CASE("RTNeural dense layer computes a forward pass", "[deps][rtneural]") {
    // 4 inputs -> 2 outputs, compile-time sized, Eigen backend.
    RTNeural::ModelT<float, 4, 2, RTNeural::DenseT<float, 4, 2>> model;

    auto& dense = model.get<0>();
    dense.setWeights(std::vector<std::vector<float>>{{1.0f, 0.0f, 0.0f, 0.0f}, //
                                                     {0.0f, 1.0f, 0.0f, 0.0f}});
    const std::array<float, 2> bias{0.5f, -0.5f};
    dense.setBias(bias.data());
    model.reset();

    const std::array<float, 4> input{1.0f, 2.0f, 3.0f, 4.0f};
    model.forward(input.data());

    const float* out = model.getOutputs();
    CHECK_THAT(static_cast<double>(out[0]), WithinAbs(1.5, 1e-6)); // 1*1 + 0.5
    CHECK_THAT(static_cast<double>(out[1]), WithinAbs(1.5, 1e-6)); // 1*2 - 0.5
}
