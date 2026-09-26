#pragma once

#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_fanout.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/tracking/state_space.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace takt4::engine {

/// The whole live chain behind one object: an input device, the `BeatEngine` tracking
/// it, and the input meter HANDOFF §5.9's window draws beside the readouts.
///
/// `takt4-cli track --device N` wires these together inline and never takes them apart:
/// the console is handed its device on the command line and exits when it is done. A
/// window cannot work that way. The operator picks a device from a list, changes their
/// mind, finds the interface is held by Traktor (R2), unplugs it — and expects the app
/// to still be there afterwards. That small state machine, and turning a driver's
/// refusal into something a caller can say out loud, is what this owns.
///
/// It lives in `core` rather than in the window for §4.2's reason: `takt4_tests` links
/// `takt4_core` alone, so a UI-side version of this could not be tested at all, and Q8's
/// headless mode would have to be written a second time.
///
/// **It deliberately does not own the transports**, exactly as `BeatEngine` does not:
/// `core/engine` stays clear of `core/output`, and whoever wants beats drains
/// `engine().popBeat()`. A caller that does want Link installs its clock on
/// `engine().setHostTimeSource()` *before* calling `start()` — §4.3's stamp is taken on
/// the audio thread, so it has to be in place before there is one.
class LiveTracker {
public:
    struct Options {
        BeatEngine::Options engine;
        audio::InputStreamOptions stream;
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

    /// The clock that stamps each hop with the host time its audio arrived at (§4.3), or
    /// null for none. Ableton Link is the one that exists; `output::OutputRunner` hands
    /// its session over as `hostTimeClock()`.
    ///
    /// Set it once, on a tracker that is stopped, and leave it: `start()` installs it on the
    /// engine before the stream is opened, which is the ordering §4.3 requires and the reason
    /// this is here rather than left to a caller to remember. `stop()` does not take it away
    /// again — it stays installed across runs — so it must outlive the tracker, not merely
    /// the run it was set for.
    void setHostTimeSource(audio::HostTimeSource* source) noexcept { hostTime_ = source; }
    audio::HostTimeSource* hostTimeSource() const noexcept { return hostTime_; }

    /// Every input device PortAudio found **the last time it looked** — at construction, or
    /// at the last `rescan()`. Not freshly enumerated each call, which is what this used to
    /// claim: PortAudio builds its device table once, so an interface switched on after takt4
    /// started was never listed, however often this was asked (the audit's H11). The indices
    /// in it are only meaningful until the next `rescan()`.
    std::vector<audio::InputDevice> devices() const;

    /// For the tests: every listing passes through `hook` before anybody sees it — a machine
    /// whose devices reorder, vanish or appear across a rescan, which no test can arrange with
    /// real hardware (the audit of 2026-09-25, H1). Empty for the machine's own list, which is
    /// all the application ever uses.
    void setDeviceListHook(std::function<void(std::vector<audio::InputDevice>&)> hook) {
        deviceListHook_ = std::move(hook);
    }

    /// Makes PortAudio enumerate the machine's devices again — an interface switched on
    /// late, or one that went away and came back. Only while stopped, because every device
    /// index changes: false, and nothing done, while a stream is open. Throws
    /// `audio::PortAudioError` if PortAudio will not come back up.
    bool rescan();

    /// Opens `device` on `selection` and starts tracking it, stopping whatever was
    /// running first — so this doubles as "switch to that one".
    ///
    /// Throws `audio::PortAudioError`, which is how R2's single-client driver says it is
    /// busy, or `std::invalid_argument` for a channel the device does not have. Nothing
    /// is left running either way, and a failed switch does not leave the previous
    /// device running: it was stopped before the attempt.
    void start(const audio::InputDevice& device, const audio::ChannelSelection& selection);

    /// Stops the stream, then the engine — that order, so the last hops in flight are
    /// still tracked. Safe to call when nothing is running.
    void stop() noexcept;

    bool running() const noexcept { return stream_ != nullptr; }

    /// What `start()` was last given, while it is still running.
    const std::optional<Running>& current() const noexcept { return current_; }

    /// The open stream, for its rates, latencies and counters. Null when stopped.
    const audio::InputStream* stream() const noexcept { return stream_.get(); }

    BeatEngine& engine() noexcept { return *engine_; }
    const BeatEngine& engine() const noexcept { return *engine_; }

    /// The input level, hop by hop, for §5.9's input meter. Drain it on the drawing
    /// timer: it is a ring like the engine's, and one nobody drains fills up and starts
    /// dropping. 50 of them arrive a second.
    bool popLevel(audio::HopLevel& out) noexcept { return meter_.pop(out); }

    /// The weight set and state space this was built with, for a status line.
    const std::filesystem::path& weightsPath() const noexcept { return weightsPath_; }
    const tracking::StateSpaceModel& stateSpace() const noexcept { return stateSpace_; }

private:
    audio::PortAudioSession session_;
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
    std::optional<Running> current_;
    /// See `setDeviceListHook`.
    std::function<void(std::vector<audio::InputDevice>&)> deviceListHook_;
};

} // namespace takt4::engine
