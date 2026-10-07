#include "core/audio/hop_fanout.hpp"

#include <stdexcept>
#include <string>

namespace takt4::audio {

HopFanout::HopFanout(std::initializer_list<HopProcessor*> processors) {
    if (processors.size() > kMaxProcessors) {
        throw std::invalid_argument("HopFanout takes at most " + std::to_string(kMaxProcessors) +
                                    " processors, got " + std::to_string(processors.size()));
    }
    for (HopProcessor* processor : processors) {
        if (processor == nullptr) {
            throw std::invalid_argument("HopFanout was given a null processor");
        }
        processors_[count_] = processor;
        ++count_;
    }
}

void HopFanout::processHop(const float* hop, std::uint64_t hopIndex) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        processors_[i]->processHop(hop, hopIndex);
    }
}

void HopFanout::beginBuffer(double firstSample, std::int64_t steadyMicros,
                            double lostSamples) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
        processors_[i]->beginBuffer(firstSample, steadyMicros, lostSamples);
    }
}

} // namespace takt4::audio
