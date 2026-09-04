#include "core/engine/live_tracker.hpp"

#include "core/model/weights.hpp"

#include <utility>

namespace takt4::engine {

LiveTracker::LiveTracker(const std::filesystem::path& weights,
                         const std::filesystem::path& stateSpace, Options options)
    : stateSpace_(tracking::StateSpaceModel::fromFile(stateSpace)), weightsPath_(weights),
      options_(options),
      // The weight set is read into a temporary and copied into the engine, which is what
      // ActivationEngine's contract allows; nothing here holds three megabytes twice.
      engine_(std::make_unique<BeatEngine>(model::ModelWeights::fromFile(weights), stateSpace_,
                                           options.engine)),
      fanout_{engine_.get(), &meter_} {}

LiveTracker::LiveTracker(const std::filesystem::path& weights,
                         const std::filesystem::path& stateSpace)
    : LiveTracker(weights, stateSpace, Options{}) {}

LiveTracker::~LiveTracker() {
    stop();
}

std::vector<audio::InputDevice> LiveTracker::devices() const {
    return audio::listInputDevices(session_);
}

void LiveTracker::start(const audio::InputDevice& device,
                        const audio::ChannelSelection& selection) {
    stop();

    // The levels of the run that just ended are still queued, and the next run's hop
    // indices start again at zero — so a consumer draining across the join would see them
    // go backwards. Throw them away with the run they belong to.
    audio::HopLevel level;
    while (meter_.pop(level)) {
    }

    // Opening can throw: a channel the device does not have (std::invalid_argument), or a
    // driver that will not give up the interface (PortAudioError, HANDOFF R2).
    auto stream =
        std::make_unique<audio::InputStream>(session_, device, selection, fanout_, options_.stream);
    // Before the stream, so the workers are draining by the time the first hop lands.
    engine_->start();
    try {
        stream->start();
    } catch (...) {
        engine_->stop();
        throw;
    }
    stream_ = std::move(stream);
    current_ = Running{device, selection};
}

void LiveTracker::stop() noexcept {
    if (stream_) {
        try {
            stream_->stop();
        } catch (...) {
            // Nothing useful is left to do about a device that will not close, and this
            // runs from the destructor. The stream is dropped either way.
        }
        stream_.reset();
    }
    // After the stream, so the hops still in the queue are tracked rather than binned.
    engine_->stop();
    current_.reset();
}

} // namespace takt4::engine
