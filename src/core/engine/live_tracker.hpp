#pragma once

#include "core/audio/audio_thread.hpp"
#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_fanout.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/tracking/state_space.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace takt4::engine {

/// The whole live chain behind one object: an input device, the `BeatEngine` tracking
/// it, and the input meter the window draws beside the readouts.
///
/// `takt4-cli track --device N` wires these together inline and never takes them apart:
/// the console is handed its device on the command line and exits when it is done. A
/// window cannot work that way. The operator picks a device from a list, changes their
/// mind, finds the interface is held by Traktor, unplugs it — and expects the app
/// to still be there afterwards. That small state machine, and turning a driver's
/// refusal into something a caller can say out loud, is what this owns.
///
/// It lives in `core` rather than in the window: `takt4_tests` links `takt4_core` alone, so a
/// UI-side version of this could not be tested at all, and a headless mode would have to be
/// written a second time.
///
/// **Every PortAudio call it makes is made on an `audio::AudioThread` of its own**: a driver
/// wedged in a stop, an open or a rescan holds that thread, and the caller — the window's thread,
/// which was frozen with PANIC in it — waits for it no longer than `Limits` says: `stop` and
/// `start` and `rescan` wait out their limits at most, and `beginStart` and `beginRescan`, which
/// the window's outage handling uses, not at all — they are asked after (`pollStart`,
/// `pollRescan`). Such a driver is `stuck()`: the stream it was stopping is let go of, detached
/// first so nothing it sends reaches the engine, and until the driver answers nothing else is
/// asked of it — `start` and `rescan`, and the two begun, throw `audio::DriverNotAnswering` at
/// once.
///
/// **It deliberately does not own the transports**, exactly as `BeatEngine` does not:
/// `core/engine` stays clear of `core/output`, and whoever wants beats drains
/// `engine().popBeat()`. A caller that does want Link installs its clock on
/// `engine().setHostTimeSource()` *before* calling `start()` — the host-time stamp is taken on
/// the audio thread, so it has to be in place before there is one.
class LiveTracker {
public:
    struct Options {
        BeatEngine::Options engine;
        audio::InputStreamOptions stream;
    };

    /// How long each kind of call to the drivers is waited for before the caller gets on
    /// without it. A healthy ASIO driver stops in milliseconds and opens in a second or two;
    /// listing the machine's devices runs the ASIO scan, which has twenty seconds of its own.
    struct Limits {
        std::chrono::milliseconds open{10'000};
        std::chrono::milliseconds stop{3'000};
        std::chrono::milliseconds rescan{30'000};
        std::chrono::milliseconds list{10'000};
    };

    /// What is running, for a caller that has to draw it. Empty until `start()`.
    struct Running {
        audio::InputDevice device;
        audio::ChannelSelection selection;
    };

    /// Reads the weight set and the state space now rather than at the first click, so a
    /// missing or corrupt asset is a startup failure with a filename in it. Brings up
    /// PortAudio, so `devices()` is answerable from here on and stays so until this is
    /// destroyed.
    ///
    /// Throws whatever the assets throw, or `audio::PortAudioError`.
    LiveTracker(const std::filesystem::path& weights, const std::filesystem::path& stateSpace,
                Options options);
    LiveTracker(const std::filesystem::path& weights, const std::filesystem::path& stateSpace);

    /// The same, from assets already loaded — which is how the application starts, because
    /// its weights and state space are compiled into it (`assets::weights()`) rather than
    /// sitting in a folder it would have to find. `weightsPath()` then reports whatever name
    /// the weight set was given rather than a path, which is what a status line wants either
    /// way: the question it answers is *which weights*, not *from where*.
    LiveTracker(model::ModelWeights weights, tracking::StateSpaceModel stateSpace,
                Options options);
    ~LiveTracker();

    LiveTracker(const LiveTracker&) = delete;
    LiveTracker& operator=(const LiveTracker&) = delete;

    /// The clock that stamps each hop with the host time its audio arrived at, or
    /// null for none. Ableton Link is the one that exists; `output::OutputRunner` hands
    /// its session over as `hostTimeClock()`.
    ///
    /// Set it once, on a tracker that is stopped, and leave it: `start()` installs it on the
    /// engine before the stream is opened, which is the ordering the stamp requires and the
    /// reason this is here rather than left to a caller to remember. `stop()` does not take it
    /// away again — it stays installed across runs — so it must outlive the tracker, not merely
    /// the run it was set for.
    void setHostTimeSource(audio::HostTimeSource* source) noexcept { hostTime_ = source; }
    audio::HostTimeSource* hostTimeSource() const noexcept { return hostTime_; }

    /// Every input device PortAudio found **the last time it looked** — at construction, or
    /// at the last `rescan()`. Not freshly enumerated each call, which is what this used to
    /// claim: PortAudio builds its device table once, so an interface switched on after takt4
    /// started was never listed, however often this was asked. The indices
    /// in it are only meaningful with the table they were read from (`deviceTable`).
    ///
    /// The list from before while a rescan is going on — the audio thread would take the
    /// question only after it — or while a driver holds the thread. Empty when the last rescan
    /// could not bring PortAudio back up: there is no table to read until one does.
    std::vector<audio::InputDevice> devices() const;

    /// Which device table PortAudio has: moved on by every rescan that has finished, one its
    /// caller stopped waiting for among them — which put a table in place that nobody has listed.
    /// A list read under another number names devices by indices that now mean something else.
    std::uint64_t deviceTable() const noexcept { return table_->load(std::memory_order_acquire); }

    /// For the tests: every listing passes through `hook` before anybody sees it — a machine
    /// whose devices reorder, vanish or appear across a rescan, which no test can arrange with
    /// real hardware. Empty for the machine's own list, which is
    /// all the application ever uses.
    void setDeviceListHook(std::function<void(std::vector<audio::InputDevice>&)> hook) {
        deviceListHook_ = std::move(hook);
    }

    /// For the tests: called on the audio thread by every open from here on, once its stream has
    /// been started and before the open is done with it — a driver slow to return from its start
    /// and calling back meanwhile, which is when an open can be given up on with its stream
    /// running. Empty for none, which is the application.
    void setStartHook(std::function<void()> hook) { startHook_ = std::move(hook); }

    /// Makes PortAudio enumerate the machine's devices again — an interface switched on
    /// late, or one that went away and came back. Only while stopped, because every device
    /// index changes: false, and nothing done, while a stream is open. Throws
    /// `audio::PortAudioError` if PortAudio will not come back up, and
    /// `audio::DriverNotAnswering` if the drivers do not answer within `Limits::rescan`.
    bool rescan();

    /// Opens `device` on `selection` and starts tracking it, stopping whatever was
    /// running first — so this doubles as "switch to that one".
    ///
    /// Throws `audio::PortAudioError`, which is how a single-client driver says it is
    /// busy, `std::invalid_argument` for a channel the device does not have, or
    /// `audio::DriverNotAnswering` when the driver does not answer within `Limits::open` — or
    /// is still holding the audio thread from before. Nothing is left running either way, and a
    /// failed switch does not leave the previous device running: it was stopped before the
    /// attempt.
    void start(const audio::InputDevice& device, const audio::ChannelSelection& selection);

    /// Detaches the stream from the engine, stops and closes it on the audio thread, and stops
    /// the engine. Returns within `Limits::stop` however the driver behaves: a driver that has
    /// not answered by then is left to finish on the audio thread, and `stuck()` says so. Safe to
    /// call when nothing is running. An open `beginStart` began and nobody has asked after is
    /// given up on: closed by the audio thread when it is made.
    void stop() noexcept;

    /// How a `beginStart` or a `beginRescan` has got on (`pollStart`, `pollRescan`).
    enum class Progress : std::uint8_t {
        /// Nothing begun, or its answer already taken.
        None,
        /// Still with the driver.
        Pending,
        /// Open and running, as `start` leaves it; or the devices listed again.
        Done,
        /// Refused, failed or given up on: why is in the `error` given.
        Failed,
    };

    /// `start`, **without waiting for the driver** — for the window, which an interface gone
    /// missing held for up to `Limits::open` at every try to bring it back, PANIC and all.
    /// Whatever was running is stopped, as `start` stops it; the open goes on on the audio
    /// thread; `pollStart` says how it went. Throws what `start` throws before a driver is asked:
    /// `audio::DriverNotAnswering` while one still holds the thread.
    void beginStart(const audio::InputDevice& device, const audio::ChannelSelection& selection);
    /// Where the open `beginStart` began has got to: `Done` is open and running, `Failed` gives
    /// why in `error`. Given up on as `start` gives up, once it has taken `Limits::open`: failed,
    /// and the driver `stuck` until it answers.
    Progress pollStart(std::string& error);
    /// An open is with the driver.
    bool starting() const noexcept { return opening_ != nullptr; }

    /// `rescan`, without waiting: false, and nothing begun, while a stream is open or being
    /// opened. Throws as `rescan` does before the drivers are asked.
    bool beginRescan();
    /// Where it has got to; given up on after `Limits::rescan`, as `rescan` gives up.
    Progress pollRescan(std::string& error);

    bool running() const noexcept { return stream_ != nullptr; }

    /// A driver did not answer a call in its time and has not answered since. See the class.
    bool stuck() const { return thread_.stuck(); }
    /// What it was asked — "stop", "open", "rescan" — or empty.
    std::string stuckOn() const { return thread_.stuckOn(); }

    /// For the tests: shorter limits than a real driver deserves, so a test of one that never
    /// answers does not wait ten seconds for it.
    void setLimits(const Limits& limits) noexcept { limits_ = limits; }
    /// For the tests: see `audio::AudioThread::setJobHook` — a hook that blocks on a job's name
    /// is a driver that does not return from that call.
    void setAudioJobHook(std::function<void(const char*)> hook) {
        thread_.setJobHook(std::move(hook));
    }

    /// What `start()` was last given, while it is still running.
    const std::optional<Running>& current() const noexcept { return current_; }

    /// The open stream, for its rates, latencies and counters. Null when stopped.
    const audio::InputStream* stream() const noexcept { return stream_.get(); }
    /// The same, to take its callbacks' load (`audio::InputStream::takeLoad`).
    audio::InputStream* stream() noexcept { return stream_.get(); }

    BeatEngine& engine() noexcept { return *engine_; }
    const BeatEngine& engine() const noexcept { return *engine_; }

    /// The input level, hop by hop, for the window's input meter. Drain it on the drawing
    /// timer: it is a ring like the engine's, and one nobody drains fills up and starts
    /// dropping. 50 of them arrive a second.
    bool popLevel(audio::HopLevel& out) noexcept { return meter_.pop(out); }

    /// The weight set and state space this was built with, for a status line.
    const std::filesystem::path& weightsPath() const noexcept { return weightsPath_; }
    const tracking::StateSpaceModel& stateSpace() const noexcept { return stateSpace_; }

private:
    /// Brings PortAudio up on the audio thread — the constructors' last step.
    void initialise();
    /// Throws `audio::DriverNotAnswering` while a driver holds the audio thread.
    void refuseWhileStuck(const char* asked) const;
    /// Where the audio thread puts a stream it has opened and started, for a caller that may have
    /// stopped waiting for it by then — which the job finds out there, under the same lock.
    struct OpenSlot;
    /// The engine readied for a run, before its stream is opened: see `start`.
    void readyForRun();
    /// A rescan, to be run on `thread_`: PortAudio ended and brought up again, and `deviceTable`
    /// moved on.
    std::function<void()> rescanJob();
    /// The open job, which puts what it opened in `slot`.
    std::function<void()> openJob(std::shared_ptr<OpenSlot> slot, const audio::InputDevice& device,
                                  const audio::ChannelSelection& selection);
    /// An open that did not finish in time, or that nobody wants any more: the slot told so, and
    /// a stream it already holds let go of on the audio thread, the way `stop` lets one go.
    void giveUp(OpenSlot& slot) noexcept;

    /// An open `beginStart` began: where its stream goes, the job, what it was for, and when.
    struct Opening {
        std::shared_ptr<OpenSlot> slot;
        audio::AudioThread::Ticket ticket;
        audio::InputDevice device;
        audio::ChannelSelection selection;
        std::chrono::steady_clock::time_point began;
    };

    /// **First**, so it is built before anything that is made on it and destroyed after it all.
    /// Mutable because asking it for the device list is a const question with a thread's answer.
    mutable audio::AudioThread thread_;
    Limits limits_;
    /// Made, restarted and ended on `thread_`, like every other PortAudio call.
    std::unique_ptr<audio::PortAudioSession> session_;
    /// The list the last look found, answered while the drivers are not answering.
    mutable std::vector<audio::InputDevice> lastDevices_;
    /// Before the engine: `BeatEngine` keeps a pointer to it and must not outlive it.
    tracking::StateSpaceModel stateSpace_;
    std::filesystem::path weightsPath_;
    Options options_;
    /// On the heap for its own header's reason — a quarter of a megabyte of rings — so
    /// that a LiveTracker on the stack is a handful of pointers.
    std::unique_ptr<BeatEngine> engine_;
    audio::HopMeter meter_;
    /// The tracker first: a late meter is a late meter, a late tracker is a dropout.
    audio::HopFanout fanout_;
    /// Not owned, and null unless a caller supplied one. See `setHostTimeSource`.
    audio::HostTimeSource* hostTime_ = nullptr;
    /// Declared after everything it points at, so it is destroyed before them.
    std::unique_ptr<audio::InputStream> stream_;
    /// The same for a stream still being opened, and a rescan going on.
    std::unique_ptr<Opening> opening_;
    audio::AudioThread::Ticket rescanning_;
    std::chrono::steady_clock::time_point rescanBegan_;
    std::optional<Running> current_;
    /// See `setDeviceListHook` and `setStartHook`.
    std::function<void(std::vector<audio::InputDevice>&)> deviceListHook_;
    std::function<void()> startHook_;
    /// See `deviceTable`. Shared with the rescan jobs, which can finish after this has gone.
    std::shared_ptr<std::atomic<std::uint64_t>> table_ =
        std::make_shared<std::atomic<std::uint64_t>>(0);
};

} // namespace takt4::engine
