#pragma once

#include "core/audio/devices.hpp"
#include "core/audio/stereo_check.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace takt4::audio {

/// Which input channels of a device feed the tracker (HANDOFF §5.1): one channel, or two
/// summed to mono. Channels are 0-based indices into the device's inputs.
struct ChannelSelection {
    std::array<int, 2> channels{0, 0};
    int count = 1; // 1 or 2

    static ChannelSelection single(int channel) noexcept { return {{channel, channel}, 1}; }
    static ChannelSelection pair(int first, int second) noexcept { return {{first, second}, 2}; }
};

/// How the selection reaches the stream.
enum class PickMode {
    Native,   // the host API opens only the selected channels (ASIO selectors, CoreAudio channel map)
    Software, // every channel is opened and the selection is sliced out of the interleaved block
};

const char* toString(PickMode mode) noexcept;

/// The one ChannelPicker abstraction of HANDOFF §5.1 with its two implementations
/// behind it. Decides how many channels to open and which, and turns each interleaved
/// input block into the mono signal the rest of the engine sees.
///
/// Software slicing is the correctness baseline: it works on every host API and is what
/// the native path is checked against, so it can be forced even where selectors exist.
class ChannelPicker {
public:
    /// Throws std::invalid_argument if the selection is not within the device's inputs
    /// or names the same channel twice.
    ChannelPicker(const InputDevice& device, const ChannelSelection& selection,
                  bool allowNative = true);

    PickMode mode() const noexcept { return mode_; }
    const ChannelSelection& selection() const noexcept { return selection_; }

    /// Channels to open the stream with: the selection's count for Native, all of the
    /// device's for Software.
    int streamChannelCount() const noexcept { return streamChannels_; }

    /// For Native: the device channel indices to hand the host API, in stream order.
    /// Empty for Software.
    std::span<const int> nativeSelectors() const noexcept;

    /// Real-time. Reads `frames` interleaved frames of streamChannelCount() channels
    /// and writes `frames` mono samples. A pair is averaged, (a + b) / 2.
    void pickMono(const float* interleaved, std::size_t frames, float* mono) const noexcept;

    /// Real-time. Adds a pair's energies and their product over `frames` frames to `sums` —
    /// what `StereoCheck` judges the pair by — and nothing for a single channel. A sample that is
    /// not a number is left out, as the mono path repairs it to silence.
    void addPairSums(const float* interleaved, std::size_t frames, StereoSums& sums) const noexcept;

private:
    ChannelSelection selection_;
    PickMode mode_;
    int streamChannels_;
    std::array<std::size_t, 2> offsets_{}; // sample offsets within one stream frame
};

} // namespace takt4::audio
