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
    ~LiveTracker();

    LiveTracker(const LiveTracker&) = delete;
    LiveTracker& operator=(const LiveTracker&) = delete;

    /// Every input device PortAudio can see, freshly enumerated each call — a device list
    /// goes stale the moment somebody plugs something in. The indices in it are only
    /// meaningful while this object lives.
    std::vector<audio::InputDevice> devices() const;

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
    /// Declared after everything it points at, so it is destroyed before them.
    std::unique_ptr<audio::InputStream> stream_;
    std::optional<Running> current_;
};

} // namespace takt4::engine
