#pragma once

#include "core/audio/hop_processor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace takt4::audio {

/// Hands every hop to several processors in turn.
///
/// `InputStream` drives one `HopProcessor`, which is all the console has ever needed: it
/// meters *or* it tracks, never both. HANDOFF §5.9's window does both at once — the BPM
/// readout and the activation trace come from the tracker, the input meter from the same
/// audio — and neither the engine nor the meter is a natural owner of the other. This
/// sits between them.
///
/// It runs on the audio thread, so §4.2 binds it and everything behind it: the
/// processors are fixed at construction, the loop allocates nothing and takes no lock,
/// and each one is bound by exactly the rules it would be under if the stream drove it
/// directly. They are called in order on that one thread — this is a fan-out, not a
/// hand-off, so a processor that dawdles delays the ones after it. Put the tracker
/// first; a meter that is late is a late meter, and a tracker that is late is a
/// dropout.
class HopFanout final : public HopProcessor {
public:
    /// Two today: the engine and the input meter. The cap is what makes `processHop`
    /// obviously allocation-free; raise it here if a third consumer ever appears.
    static constexpr std::size_t kMaxProcessors = 4;

    /// Every processor must outlive this and none may be null. Throws
    /// `std::invalid_argument` on either, or on more than `kMaxProcessors`.
    HopFanout(std::initializer_list<HopProcessor*> processors);

    void processHop(const float* hop, std::uint64_t hopIndex) noexcept override;
    void beginBuffer(double firstSample, std::int64_t steadyMicros,
                     double lostSamples) noexcept override;

    std::size_t size() const noexcept { return count_; }

private:
    std::array<HopProcessor*, kMaxProcessors> processors_{};
    std::size_t count_ = 0;
};

} // namespace takt4::audio
