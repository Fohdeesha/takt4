#include "core/audio/channel_picker.hpp"

#include <stdexcept>
#include <string>

namespace takt4::audio {

const char* toString(PickMode mode) noexcept {
    switch (mode) {
    case PickMode::Native:
        return "native";
    case PickMode::Software:
        return "software";
    }
    return "?";
}

ChannelPicker::ChannelPicker(const InputDevice& device, const ChannelSelection& selection,
                             bool allowNative)
    : selection_(selection),
      mode_(allowNative && hasNativeChannelSelection(device.hostApi) ? PickMode::Native
                                                                      : PickMode::Software),
      streamChannels_(0) {
    if (selection.count != 1 && selection.count != 2) {
        throw std::invalid_argument("channel selection must name one or two channels");
    }
    for (int i = 0; i < selection.count; ++i) {
        const int channel = selection.channels[static_cast<std::size_t>(i)];
        if (channel < 0 || channel >= device.maxInputChannels) {
            throw std::invalid_argument("input channel " + std::to_string(channel + 1) +
                                        " is outside " + device.name + " (" +
                                        std::to_string(device.maxInputChannels) + " inputs)");
        }
    }
    if (selection.count == 2 && selection.channels[0] == selection.channels[1]) {
        throw std::invalid_argument("a channel pair needs two different channels");
    }

    if (mode_ == PickMode::Native) {
        // The stream carries only the selected channels, in selection order.
        streamChannels_ = selection.count;
        offsets_ = {0, 1};
    } else {
        streamChannels_ = device.maxInputChannels;
        offsets_ = {static_cast<std::size_t>(selection.channels[0]),
                    static_cast<std::size_t>(selection.channels[1])};
    }
}

std::span<const int> ChannelPicker::nativeSelectors() const noexcept {
    if (mode_ != PickMode::Native) {
        return {};
    }
    return {selection_.channels.data(), static_cast<std::size_t>(selection_.count)};
}

void ChannelPicker::pickMono(const float* interleaved, std::size_t frames, float* mono) const noexcept {
    const auto stride = static_cast<std::size_t>(streamChannels_);
    const float* src = interleaved;
    if (selection_.count == 1) {
        const std::size_t offset = offsets_[0];
        for (std::size_t i = 0; i < frames; ++i, src += stride) {
            mono[i] = src[offset];
        }
    } else {
        const std::size_t a = offsets_[0];
        const std::size_t b = offsets_[1];
        for (std::size_t i = 0; i < frames; ++i, src += stride) {
            mono[i] = 0.5f * (src[a] + src[b]);
        }
    }
}

} // namespace takt4::audio
