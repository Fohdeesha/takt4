#include "core/engine/live_tracker.hpp"

#include "core/model/weights.hpp"

#include <mutex>
#include <utility>

namespace takt4::engine {

namespace {

/// "3 s" or "250 ms", for a message about how long a driver was given.
std::string spoken(std::chrono::milliseconds limit) {
    return limit.count() % 1000 == 0 ? std::to_string(limit.count() / 1000) + " s"
                                     : std::to_string(limit.count()) + " ms";
}

/// Where the audio thread puts a stream it has opened and started, for a caller that may have
/// stopped waiting for it by then — which the job finds out here, under the same lock.
struct OpenSlot {
    std::mutex mutex;
    std::unique_ptr<audio::InputStream> stream;
    bool abandoned = false;
};

} // namespace

LiveTracker::LiveTracker(const std::filesystem::path& weights,
                         const std::filesystem::path& stateSpace, Options options)
    : stateSpace_(tracking::StateSpaceModel::fromFile(stateSpace)), weightsPath_(weights),
      options_(options),
      // The weight set is read into a temporary and copied into the engine, which is what
      // ActivationEngine's contract allows; nothing here holds three megabytes twice.
      engine_(std::make_unique<BeatEngine>(model::ModelWeights::fromFile(weights), stateSpace_,
                                           options.engine)),
      fanout_{engine_.get(), &meter_} {
    // PortAudio is brought up on the audio thread, like every call after it.
    initialise();
}

LiveTracker::LiveTracker(const std::filesystem::path& weights,
                         const std::filesystem::path& stateSpace)
    : LiveTracker(weights, stateSpace, Options{}) {}

LiveTracker::LiveTracker(model::ModelWeights weights, tracking::StateSpaceModel stateSpace,
                         Options options)
    // `weightsPath_` is declared after `stateSpace_` and before `engine_`, so it is read out
    // of `weights` while `weights` is still whole.
    : stateSpace_(std::move(stateSpace)), weightsPath_(weights.path()), options_(options),
      engine_(std::make_unique<BeatEngine>(weights, stateSpace_, options.engine)),
      fanout_{engine_.get(), &meter_} {
    initialise();
}

void LiveTracker::initialise() {
    // Into a slot the job owns rather than into `session_`: a job that outlives its limit goes on
    // running after a constructor that threw has taken this object away.
    auto made = std::make_shared<std::unique_ptr<audio::PortAudioSession>>();
    if (!thread_.run(
            "initialise", [made] { *made = std::make_unique<audio::PortAudioSession>(); },
            limits_.rescan)) {
        throw audio::DriverNotAnswering("the audio drivers did not answer while takt4 started (" +
                                        spoken(limits_.rescan) + ")");
    }
    session_ = std::move(*made);
}

LiveTracker::~LiveTracker() {
    stop();
    if (thread_.stuck()) {
        // A driver holding the audio thread: PortAudio cannot be ended from anywhere else, and
        // not from there until it lets go. Left to it, as the thread is.
        (void)session_.release();
        return;
    }
    // Ended where it was begun. A shared pointer, so the job owns it however long it takes.
    std::shared_ptr<audio::PortAudioSession> session(std::move(session_));
    (void)thread_.handOver(
        "terminate", [session = std::move(session)]() mutable { session.reset(); }, limits_.stop);
}

void LiveTracker::refuseWhileStuck(const char* asked) const {
    if (thread_.stuck()) {
        throw audio::DriverNotAnswering("the audio driver has not answered since it was asked to " +
                                        thread_.stuckOn() + ", so it cannot be asked to " + asked +
                                        " until it does");
    }
}

std::vector<audio::InputDevice> LiveTracker::devices() const {
    // A heap copy for the job to fill: one that outlives its time goes on running after this has
    // returned, and must not be writing into a list on a stack that is gone.
    auto found = std::make_shared<std::vector<audio::InputDevice>>();
    bool answered = false;
    if (!thread_.stuck()) {
        answered = thread_.run(
            "list", [found, session = session_.get()] { *found = audio::listInputDevices(*session); },
            limits_.list);
    }
    // The last list the drivers answered with, while they are not answering.
    std::vector<audio::InputDevice> list = answered ? *found : lastDevices_;
    if (answered) {
        lastDevices_ = list;
    }
    if (deviceListHook_) {
        deviceListHook_(list);
    }
    return list;
}

bool LiveTracker::rescan() {
    if (stream_) {
        return false;
    }
    refuseWhileStuck("look for devices again");
    if (!thread_.run("rescan", [session = session_.get()] { session->restart(); }, limits_.rescan)) {
        throw audio::DriverNotAnswering("the audio drivers did not answer while being listed again (" +
                                        spoken(limits_.rescan) + ")");
    }
    return true;
}

void LiveTracker::start(const audio::InputDevice& device,
                        const audio::ChannelSelection& selection) {
    stop();
    refuseWhileStuck("open an input");

    // The levels of the run that just ended are still queued, and the next run's hop
    // indices start again at zero — so a consumer draining across the join would see them
    // go backwards. Throw them away with the run they belong to.
    audio::HopLevel level;
    while (meter_.pop(level)) {
    }

    // §4.3's stamp is taken on the audio thread, so the clock has to be in place before
    // there is one. A restarted stream begins its sample counter again, and the filter's
    // regression is fitted to the old one, so it is forgotten with the run it belonged to.
    if (hostTime_ != nullptr) {
        hostTime_->resetHostTimeFilter();
    }
    engine_->setHostTimeSource(hostTime_);
    // Before the stream, so the workers are draining by the time the first hop lands.
    engine_->start();

    // Opened and started on the audio thread, where it will be stopped and closed. Opening can
    // throw — a channel the device does not have (std::invalid_argument), or a driver that will
    // not give up the interface (PortAudioError, HANDOFF R2) — and that reaches here.
    auto slot = std::make_shared<OpenSlot>();
    bool opened = false;
    try {
        opened = thread_.run(
            "open",
            [slot, session = session_.get(), device, selection, processor = &fanout_,
             options = options_.stream] {
                auto stream = std::make_unique<audio::InputStream>(*session, device, selection,
                                                                   *processor, options);
                {
                    const std::lock_guard<std::mutex> lock(slot->mutex);
                    if (slot->abandoned) {
                        // Nobody is waiting for it any more: never started, closed here.
                        stream->detach();
                        return;
                    }
                }
                stream->start();
                const std::lock_guard<std::mutex> lock(slot->mutex);
                if (slot->abandoned) {
                    // Given up on while it started: detached before anything reaches the engine
                    // the caller has moved on from, and stopped and closed here, on this thread.
                    stream->detach();
                    try {
                        stream->stop();
                    } catch (...) {
                    }
                    return;
                }
                slot->stream = std::move(stream);
            },
            limits_.open);
    } catch (...) {
        engine_->stop();
        throw;
    }
    if (!opened) {
        std::unique_ptr<audio::InputStream> late;
        {
            const std::lock_guard<std::mutex> lock(slot->mutex);
            slot->abandoned = true;
            late = std::move(slot->stream);
        }
        if (late) {
            // It opened in the moment between the limit and here: let it go the way `stop` does.
            late->detach();
            std::shared_ptr<audio::InputStream> owned(std::move(late));
            (void)thread_.handOver(
                "stop",
                [owned = std::move(owned)]() mutable {
                    try {
                        owned->stop();
                    } catch (...) {
                    }
                    owned.reset();
                },
                limits_.stop);
        }
        engine_->stop();
        throw audio::DriverNotAnswering(device.name + " did not answer within " +
                                        spoken(limits_.open) + " of being opened");
    }
    {
        const std::lock_guard<std::mutex> lock(slot->mutex);
        stream_ = std::move(slot->stream);
    }
    current_ = Running{device, selection};
}

void LiveTracker::stop() noexcept {
    if (stream_) {
        // **Off the engine first, on this thread**: no PortAudio call, and a callback's length at
        // most. From here the driver can take as long as it likes over the stop, or never finish
        // it, and nothing it sends reaches the engine this goes on to stop and may start again.
        stream_->detach();
        // Then stopped and closed where it was opened. The job owns it — a stream closed on
        // this thread, by the last owner letting go here, would be the call this exists to keep
        // off it.
        std::shared_ptr<audio::InputStream> owned(std::move(stream_));
        try {
            (void)thread_.handOver(
                "stop",
                // Moved in, so the job holds the only share: whichever way it goes, the stream
                // is let go of on the audio thread.
                [owned = std::move(owned)]() mutable {
                    try {
                        owned->stop();
                    } catch (...) {
                        // A device that will not stop is closed anyway, below.
                    }
                    owned.reset();
                },
                limits_.stop);
        } catch (...) {
            // Nothing is thrown by a job that catches everything; nothing useful to do if it were.
        }
    }
    // After the stream, so the hops still in the queue are tracked rather than binned.
    engine_->stop();
    current_.reset();
}

} // namespace takt4::engine
