#include "core/model/beat_model.hpp"

#include <RTNeural/RTNeural.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

// This translation unit includes <RTNeural/RTNeural.h>, so it must not also see
// <nlohmann/json.hpp> — RTNeural bundles an older copy (cmake/deps.cmake).

namespace takt4::model {

namespace {

/// PyTorch keeps a Linear's weight as (out, in) and RTNeural's Dense wants
/// weights[out][in], so this is a reshape and nothing more.
std::vector<std::vector<float>> rows(std::span<const float> flat, std::size_t outer, std::size_t inner) {
    std::vector<std::vector<float>> out(outer, std::vector<float>(inner));
    for (std::size_t r = 0; r < outer; ++r) {
        std::copy_n(flat.begin() + static_cast<std::ptrdiff_t>(r * inner), inner, out[r].begin());
    }
    return out;
}

/// The transpose: RTNeural's LSTM wants weights[in][4 · out] where PyTorch stores
/// (4 · out, in). Both put the gates in the order input, forget, cell, output, so only
/// the two indices swap — RTNeural::torch_helpers::loadLSTM does exactly this.
std::vector<std::vector<float>> transposed(std::span<const float> flat, std::size_t outer, std::size_t inner) {
    std::vector<std::vector<float>> out(inner, std::vector<float>(outer));
    for (std::size_t r = 0; r < outer; ++r) {
        for (std::size_t c = 0; c < inner; ++c) {
            out[c][r] = flat[r * inner + c];
        }
    }
    return out;
}

} // namespace

struct BeatModel::Impl {
    // Everything after the convolution block. RTNeural has no pooling layer and its
    // Conv1D convolves along time rather than across one frame's features, so the
    // 22-parameter front of the network is done here by hand instead; see extract().
    using Net = RTNeural::ModelT<float, kDenseIn, kNumClasses,
                                 RTNeural::DenseT<float, kDenseIn, kHidden>,
                                 RTNeural::LSTMLayerT<float, kHidden, kHidden>,
                                 RTNeural::LSTMLayerT<float, kHidden, kHidden>,
                                 RTNeural::LSTMLayerT<float, kHidden, kHidden>,
                                 RTNeural::LSTMLayerT<float, kHidden, kHidden>,
                                 RTNeural::DenseT<float, kHidden, kNumClasses>>;
    static_assert(kLstmLayers == 4, "the layer list above is written out; keep it in step");

    Net net;
    std::array<float, kConvFilters * kKernelSize> convWeight{};
    std::array<float, kConvFilters> convBias{};

    // ModelT::forward() maps its argument as Eigen::Aligned16, so this has to be.
    // Debug builds assert on an unaligned buffer; Release builds load it silently and
    // are simply slower. The padding MSVC warns about (C4324) is the point.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    alignas(RTNEURAL_DEFAULT_ALIGNMENT) std::array<float, kDenseIn> pooled{};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    explicit Impl(const ModelWeights& weights) {
        std::copy(weights.convWeight().begin(), weights.convWeight().end(), convWeight.begin());
        std::copy(weights.convBias().begin(), weights.convBias().end(), convBias.begin());

        net.get<0>().setWeights(rows(weights.denseWeight(), kHidden, kDenseIn));
        net.get<0>().setBias(weights.denseBias().data());

        setLstm<1>(weights.lstm(0));
        setLstm<2>(weights.lstm(1));
        setLstm<3>(weights.lstm(2));
        setLstm<4>(weights.lstm(3));

        net.get<5>().setWeights(rows(weights.outputWeight(), kNumClasses, kHidden));
        net.get<5>().setBias(weights.outputBias().data());
        net.reset();
    }

    template <int Index>
    void setLstm(const LstmWeights& layer) {
        net.get<Index>().setWVals(transposed(layer.weightIh, kGates * kHidden, kHidden));
        net.get<Index>().setUVals(transposed(layer.weightHh, kGates * kHidden, kHidden));
        // PyTorch adds both bias vectors to the same sum; RTNeural holds one.
        std::vector<float> bias(kGates * kHidden);
        for (std::size_t i = 0; i < bias.size(); ++i) {
            bias[i] = layer.biasIh[i] + layer.biasHh[i];
        }
        net.get<Index>().setBVals(bias);
    }

    /// Conv1d(1, 2, 10) -> ReLU -> MaxPool1d(2) -> flatten, as PyTorch runs it: the
    /// pool takes 279 convolution outputs two at a time and leaves the last one, and
    /// the flatten is filter-major, filter 0's 139 values then filter 1's.
    void extract(std::span<const float, kFeatureDim> frame) noexcept {
        for (std::size_t f = 0; f < kConvFilters; ++f) {
            const float* kernel = convWeight.data() + f * kKernelSize;
            const float bias = convBias[f];
            float* out = pooled.data() + f * kPooledLength;
            for (std::size_t p = 0; p < kPooledLength; ++p) {
                float best = 0.0f;
                for (std::size_t half = 0; half < 2; ++half) {
                    float sum = bias;
                    const float* window = frame.data() + 2 * p + half;
                    for (std::size_t k = 0; k < kKernelSize; ++k) {
                        sum += kernel[k] * window[k];
                    }
                    best = std::max(best, sum); // max after the ReLU, which never goes below zero
                }
                out[p] = best;
            }
        }
    }
};

BeatModel::BeatModel(const ModelWeights& weights) : impl_(std::make_unique<Impl>(weights)) {}

BeatModel::~BeatModel() = default;
BeatModel::BeatModel(BeatModel&&) noexcept = default;
BeatModel& BeatModel::operator=(BeatModel&&) noexcept = default;

void BeatModel::reset() noexcept {
    impl_->net.reset();
    framesProcessed_ = 0;
}

BeatModel::Activation BeatModel::process(std::span<const float, kFeatureDim> frame) noexcept {
    impl_->extract(frame);
    impl_->net.forward(impl_->pooled.data());
    const float* out = impl_->net.getOutputs();

    Activation activation;
    std::copy_n(out, kNumClasses, activation.logits.begin());
    const float peak = *std::max_element(activation.logits.begin(), activation.logits.end());
    float total = 0.0f;
    for (std::size_t c = 0; c < kNumClasses; ++c) {
        activation.probabilities[c] = std::exp(activation.logits[c] - peak);
        total += activation.probabilities[c];
    }
    for (float& p : activation.probabilities) {
        p /= total;
    }
    ++framesProcessed_;
    return activation;
}

} // namespace takt4::model
