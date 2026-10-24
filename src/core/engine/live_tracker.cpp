#include "core/engine/live_tracker.hpp"

#include "core/model/weights.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace takt4::engine {

namespace {

/// "3 s" or "250 ms", for a message about how long a driver was given.
std::string spoken(std::chrono::milliseconds limit) {
    return limit.count() % 1000 == 0 ? std::to_string(limit.count() / 1000) + " s"
                                     : std::to_string(limit.count()) + " ms";
}

} // namespace

struct LiveTracker::OpenSlot {
    std::mutex mutex;
    std::unique_ptr<audio::InputStream> stream;
    bool abandoned = false;
    /// The stream while it is being started, which a driver may call back from before `start`
    /// returns: for `giveUp` to shut, so nothing it sends reaches an engine that may be going.
    audio::InputStream* starting = nullptr;
};

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
    bool none = false;
    // A rescan going on has the thread, and the question would wait behind it.
    const bool rescanning = rescanning_ && !rescanning_.done();
    if (!thread_.stuck() && !rescanning) {
        try {
            answered = thread_.run(
                "list",
                [found, session = session_.get()] { *found = audio::listInputDevices(*session); },
                limits_.list);
        } catch (...) {
            // No table to read: PortAudio could not be brought back up by the last rescan, which
            // said why. Thrown into the window it ended takt4 — an IMPORT after it, or the
            // outage's next look at the list.
            none = true;
        }
    }
    // The last list the drivers answered with, while they are not answering.
    std::vector<audio::InputDevice> list = answered ? *found
                                           : none   ? std::vector<audio::InputDevice>{}
                                                    : lastDevices_;
    if (answered || none) {
        lastDevices_ = list;
    }
    if (deviceListHook_) {
        deviceListHook_(list);
    }
    return list;
}

bool LiveTracker::rescan() {
    if (stream_ || opening_) {
        return false;
    }
    refuseWhileStuck("look for devices again");
    if (!thread_.run("rescan", rescanJob(), limits_.rescan)) {
        throw audio::DriverNotAnswering("the audio drivers did not answer while being listed again (" +
                                        spoken(limits_.rescan) + ")");
    }
    return true;
}

std::function<void()> LiveTracker::rescanJob() {
    // The table is another one once PortAudio has been ended, whether or not it comes back up —
    // and said so even when the caller has stopped waiting, which is when nobody else would.
    return [session = session_.get(), table = table_] {
        try {
            session->restart();
        } catch (...) {
            table->fetch_add(1, std::memory_order_acq_rel);
            throw;
        }
        table->fetch_add(1, std::memory_order_acq_rel);
    };
}

void LiveTracker::readyForRun() {
    // The levels of the run that just ended are still queued, and the next run's hop
    // indices start again at zero — so a consumer draining across the join would see them
    // go backwards. Throw them away with the run they belong to.
    audio::HopLevel level;
    while (meter_.pop(level)) {
    }

    // The host-time stamp is taken on the audio thread, so the clock has to be in place before
    // there is one. A restarted stream begins its sample counter again, and the filter's
    // regression is fitted to the old one, so it is forgotten with the run it belonged to.
    if (hostTime_ != nullptr) {
        hostTime_->resetHostTimeFilter();
    }
    engine_->setHostTimeSource(hostTime_);
    // Before the stream, so the workers are draining by the time the first hop lands.
    engine_->start();
}

std::function<void()> LiveTracker::openJob(std::shared_ptr<OpenSlot> slot,
                                           const audio::InputDevice& device,
                                           const audio::ChannelSelection& selection) {
    // Opened and started on the audio thread, where it will be stopped and closed. Opening can
    // throw — a channel the device does not have (std::invalid_argument), or a driver that will
    // not give up the interface (PortAudioError) — and that reaches the caller.
    //
    // The processor by address, though the job can outlive the tracker: everything the stream
    // sends reaches it through the stream's gate, and `giveUp` — which the tracker's way out goes
    // through — shuts that gate on a stream still starting, before anything it points at goes.
    return [slot = std::move(slot), session = session_.get(), device, selection,
            processor = &fanout_, options = options_.stream, hook = startHook_] {
        auto stream =
            std::make_unique<audio::InputStream>(*session, device, selection, *processor, options);
        {
            const std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->abandoned) {
                // Nobody is waiting for it any more: never started, closed here.
                stream->detach();
                return;
            }
            slot->starting = stream.get();
        }
        try {
            stream->start();
        } catch (...) {
            const std::lock_guard<std::mutex> lock(slot->mutex);
            slot->starting = nullptr;
            throw;
        }
        if (hook) {
            hook();
        }
        const std::lock_guard<std::mutex> lock(slot->mutex);
        slot->starting = nullptr;
        if (slot->abandoned) {
            // Given up on while it started: detached before anything reaches the engine the
            // caller has moved on from, and stopped and closed here, on this thread.
            stream->detach();
            try {
                stream->stop();
            } catch (...) {
            }
            return;
        }
        slot->stream = std::move(stream);
    };
}

void LiveTracker::giveUp(OpenSlot& slot) noexcept {
    std::unique_ptr<audio::InputStream> late;
    {
        const std::lock_guard<std::mutex> lock(slot.mutex);
        slot.abandoned = true;
        if (slot.starting != nullptr) {
            // Starting this moment: whatever it sends from here on reaches nothing.
            slot.starting->detach();
        }
        late = std::move(slot.stream);
    }
    if (!late) {
        return; // the job closes whatever it opens from here
    }
    // It opened in the moment between the limit and here: let it go the way `stop` does.
    late->detach();
    std::shared_ptr<audio::InputStream> owned(std::move(late));
    try {
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
    } catch (...) {
        // Nothing is thrown by a job that catches everything.
    }
}

void LiveTracker::start(const audio::InputDevice& device,
                        const audio::ChannelSelection& selection) {
    stop();
    refuseWhileStuck("open an input");
    readyForRun();
    auto slot = std::make_shared<OpenSlot>();
    bool opened = false;
    try {
        opened = thread_.run("open", openJob(slot, device, selection), limits_.open);
    } catch (...) {
        engine_->stop();
        throw;
    }
    if (!opened) {
        giveUp(*slot);
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

void LiveTracker::beginStart(const audio::InputDevice& device,
                             const audio::ChannelSelection& selection) {
    stop();
    refuseWhileStuck("open an input");
    readyForRun();
    auto opening = std::make_unique<Opening>();
    opening->slot = std::make_shared<OpenSlot>();
    opening->ticket = thread_.post("open", openJob(opening->slot, device, selection));
    if (!opening->ticket) {
        // A driver began holding the thread between the check above and here.
        engine_->stop();
        refuseWhileStuck("open an input");
        throw audio::DriverNotAnswering("the audio thread would not take the open");
    }
    opening->device = device;
    opening->selection = selection;
    opening->began = std::chrono::steady_clock::now();
    opening_ = std::move(opening);
}

LiveTracker::Progress LiveTracker::pollStart(std::string& error) {
    if (!opening_) {
        return Progress::None;
    }
    if (!opening_->ticket.done()) {
        if (std::chrono::steady_clock::now() - opening_->began < limits_.open) {
            return Progress::Pending;
        }
        // Given up on, as `start` gives up: the driver holds the thread until it answers.
        opening_->ticket.abandon();
        giveUp(*opening_->slot);
        error = opening_->device.name + " did not answer within " + spoken(limits_.open) +
                " of being opened";
        opening_.reset();
        engine_->stop();
        return Progress::Failed;
    }
    try {
        opening_->ticket.rethrow();
    } catch (const std::exception& e) {
        error = e.what();
        opening_.reset();
        engine_->stop();
        return Progress::Failed;
    } catch (...) {
        error = "the driver would not open it";
        opening_.reset();
        engine_->stop();
        return Progress::Failed;
    }
    {
        const std::lock_guard<std::mutex> lock(opening_->slot->mutex);
        stream_ = std::move(opening_->slot->stream);
    }
    current_ = Running{opening_->device, opening_->selection};
    opening_.reset();
    return Progress::Done;
}

bool LiveTracker::beginRescan() {
    if (rescanning_ && rescanning_.done()) {
        rescanning_ = {}; // one nobody asked after — the outage it was for has ended
    }
    if (stream_ || opening_ || rescanning_) {
        return false;
    }
    refuseWhileStuck("look for devices again");
    rescanning_ = thread_.post("rescan", rescanJob());
    if (!rescanning_) {
        refuseWhileStuck("look for devices again");
        return false;
    }
    rescanBegan_ = std::chrono::steady_clock::now();
    return true;
}

LiveTracker::Progress LiveTracker::pollRescan(std::string& error) {
    if (!rescanning_) {
        return Progress::None;
    }
    if (!rescanning_.done()) {
        if (std::chrono::steady_clock::now() - rescanBegan_ < limits_.rescan) {
            return Progress::Pending;
        }
        rescanning_.abandon();
        rescanning_ = {};
        error = "the audio drivers did not answer while being listed again (" +
                spoken(limits_.rescan) + ")";
        return Progress::Failed;
    }
    const audio::AudioThread::Ticket done = std::exchange(rescanning_, {});
    try {
        done.rethrow();
    } catch (const std::exception& e) {
        error = e.what();
        return Progress::Failed;
    } catch (...) {
        error = "the audio drivers could not be listed again";
        return Progress::Failed;
    }
    return Progress::Done;
}

void LiveTracker::stop() noexcept {
    if (opening_) {
        // An open nobody will now ask after: whatever it opens is closed where it was made.
        giveUp(*opening_->slot);
        opening_.reset();
    }
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
