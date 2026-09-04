#include "ui/app.hpp"

#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "ui/window_state.hpp"

#include "main_window.h" // generated from main_window.slint

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace takt4::ui {
namespace {

using namespace std::chrono_literals;

/// How often the window drains the engine's rings and redraws. HANDOFF §7.5: "Push
/// audio-side results through a lock-free ring and drain on a UI timer — do not queue one
/// closure per audio callback." 30 Hz is slower than the 50 Hz the frames arrive at, so
/// every tick finds one or two waiting and none is ever missed; the ring holds ten
/// seconds of them, so even a stalled event loop loses nothing.
constexpr auto kRedrawInterval = 33ms;

/// How far the peak indicator falls back each tick — slow enough that a moment of
/// clipping is still on screen when the operator looks up.
constexpr float kPeakDecay = 0.88f;

std::filesystem::path weightsPath() {
    return std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin";
}

std::filesystem::path stateSpacePath() {
    return std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";
}

slint::SharedString shared(const std::string& text) {
    return slint::SharedString(text);
}

/// Everything the window needs that is not the window: the tracker behind it, the model
/// behind its trace, and the one timer that drains the engine's rings.
///
/// Nothing here is ever reached from the audio thread, which is the whole of §7.5's
/// rule. The engine writes into lock-free rings from its own threads; this reads them on
/// the UI thread, on a timer, and touches a Slint property nowhere else.
class WindowController {
public:
    explicit WindowController(engine::LiveTracker& tracker)
        : tracker_(tracker), window_(MainWindow::create()), trace_(kTraceLength),
          traceModel_(std::make_shared<slint::VectorModel<TracePoint>>(
              std::vector<TracePoint>(kTraceLength))) {
        window_->set_trace(traceModel_);

        window_->on_device_picked([this](int index) { pickDevice(index); });
        window_->on_channel_picked([this](int index) { channel_ = index; });
        window_->on_toggle_run([this] { toggleRun(); });

        refreshDevices();
        publishStopped();

        timer_.start(slint::TimerMode::Repeated, kRedrawInterval, [this] { tick(); });
    }

    void run() { window_->run(); }

private:
    void refreshDevices() {
        devices_ = tracker_.devices();
        auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (const audio::InputDevice& device : devices_) {
            names->push_back(shared(describeDevice(device)));
        }
        window_->set_devices(names);

        if (devices_.empty()) {
            device_ = -1;
            window_->set_channels(std::make_shared<slint::VectorModel<slint::SharedString>>());
            setStatus("No input device. Connect an interface and start takt4 again.", true);
            return;
        }

        // The most useful device to land on: a real input before a loopback of the
        // speakers (Q3 offers loopback as a way round a busy interface, not as the
        // normal path), then a host API that can pick channels natively, then the
        // machine's own default input, then the one with the most inputs.
        const auto rank = [](const audio::InputDevice& device) {
            return std::make_tuple(!device.isLoopback,
                                   audio::hasNativeChannelSelection(device.hostApi),
                                   device.isDefaultInput, device.maxInputChannels);
        };
        std::size_t best = 0;
        for (std::size_t i = 1; i < devices_.size(); ++i) {
            if (rank(devices_[i]) > rank(devices_[best])) {
                best = i;
            }
        }
        window_->set_device_index(static_cast<int>(best));
        pickDevice(static_cast<int>(best));
    }

    void pickDevice(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= devices_.size()) {
            return;
        }
        device_ = index;
        channel_ = 0;
        const audio::InputDevice& device = devices_[static_cast<std::size_t>(index)];
        auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (int c = 0; c < device.maxInputChannels; ++c) {
            names->push_back(shared(describeChannel(device, c)));
        }
        window_->set_channels(names);
        window_->set_channel_index(0);
        if (statusIsError_) {
            // Whatever went wrong was about the device that is no longer selected.
            setStatus("Pick an input and press Start.", false);
        }
    }

    void toggleRun() {
        if (tracker_.running()) {
            tracker_.stop();
            publishStopped();
            return;
        }
        if (device_ < 0) {
            return;
        }
        const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
        try {
            tracker_.start(device, audio::ChannelSelection::single(channel_));
        } catch (const audio::PortAudioError& e) {
            // HANDOFF Q3 and R2: most ASIO drivers are single-client, and a busy
            // interface is the exact situation takt4 exists for. A PortAudio error code
            // is no use to anyone standing at a laptop, so say what it means and what the
            // ways round it are.
            setStatus("Cannot open " + device.name + ": " + e.what() +
                          ". If another application already has this interface, most ASIO "
                          "drivers will not share it — use the same interface's WASAPI "
                          "loopback, a virtual audio cable, or a spare physical input.",
                      true);
            publishStopped();
            return;
        } catch (const std::exception& e) {
            setStatus("Cannot open " + device.name + ": " + e.what(), true);
            publishStopped();
            return;
        }
        std::fill(trace_.begin(), trace_.end(), TracePoint{});
        publishTrace();
        peak_ = 0.0f;
        window_->set_running(true);
        publishOpenStream();
    }

    void publishStopped() {
        window_->set_running(false);
        publishIdleReadouts(*window_);
        publishOptions();
        if (!devices_.empty() && !statusIsError_) {
            setStatus("takt4 " + buildInfo().version + " — pick an input and press Start.", false);
        }
    }

    /// What the signal goes through before a beat can be called: the number §5.5's
    /// latency offset exists to cancel, and the first thing to look at when downstream is
    /// late. Only knowable once a stream is actually open.
    void publishOpenStream() {
        const audio::InputStream* stream = tracker_.stream();
        if (stream == nullptr || !tracker_.current()) {
            return;
        }
        const double resamplerMs =
            1000.0 * static_cast<double>(stream->resamplerDelayFrames()) / stream->sampleRate();
        const engine::LiveTracker::Running& running = *tracker_.current();
        setStatus("In " + std::to_string(running.selection.channels[0] + 1) + " of " +
                      running.device.name + "  ·  " + fixed(stream->sampleRate(), 0) + " Hz -> " +
                      fixed(audio::kInternalSampleRate, 0) + " Hz  ·  " +
                      audio::toString(stream->picker().mode()) + " pick  ·  latency " +
                      fixed(stream->inputLatencySeconds() * 1000.0, 1) + " ms input + " +
                      fixed(resamplerMs, 1) + " ms resampler + 40.0 ms centred framing",
                  false);
    }

    /// §5.5's settings as the tracker actually holds them. Read from the engine every
    /// time and never from a copy: a tap moves the fold window underneath anyone keeping
    /// one (§7 deviation 8), and this window exists to show that window honestly.
    void publishOptions() { publishTempoOptions(*window_, tracker_.engine().tempoOptions()); }

    void setStatus(const std::string& text, bool error) {
        statusIsError_ = error;
        window_->set_status(shared(text));
        window_->set_status_is_error(error);
    }

    void tick() {
        if (!tracker_.running()) {
            return;
        }

        // Every frame since the last tick joins the trace, newest at the right.
        bool moved = false;
        engine::EngineFrame frame;
        while (tracker_.engine().popFrame(frame)) {
            std::rotate(trace_.begin(), trace_.begin() + 1, trace_.end());
            trace_.back() = tracePoint(frame);
            moved = true;
        }
        if (moved) {
            publishTrace();
        }

        // The beats are drained and dropped. This window says nothing a beat carries that
        // the published state does not already have — but a ring nobody drains fills up,
        // and the engine would rightly start counting beats lost. When the outputs arrive
        // this is where they are driven from, or from the output thread that replaces this
        // loop (§8 item 2).
        engine::EngineBeat beat;
        while (tracker_.engine().popBeat(beat)) {
        }

        publishState();
        publishLevels();
    }

    void publishTrace() {
        for (std::size_t i = 0; i < trace_.size(); ++i) {
            traceModel_->set_row_data(i, trace_[i]);
        }
    }

    void publishState() {
        publishTempoState(*window_, tracker_.engine().state());
        publishOptions();
    }

    void publishLevels() {
        float loudest = -1.0f; // negative: no hop arrived this tick
        float peak = 0.0f;
        audio::HopLevel level;
        while (tracker_.popLevel(level)) {
            loudest = std::max(loudest, level.rms);
            peak = std::max(peak, level.peak);
        }
        peak_ = std::max(peak, peak_ * kPeakDecay);
        if (loudest < 0.0f) {
            // Nothing arrived this tick: let the peak fall, leave the reading where it
            // was rather than flashing to silence.
            window_->set_input_peak(peak_);
            return;
        }
        publishInput(*window_, loudest, peak_);
    }

    engine::LiveTracker& tracker_;
    slint::ComponentHandle<MainWindow> window_;
    slint::Timer timer_;

    std::vector<audio::InputDevice> devices_;
    int device_ = -1;
    int channel_ = 0;

    /// The trace as a plain buffer, oldest first, mirrored into the model each tick.
    std::vector<TracePoint> trace_;
    std::shared_ptr<slint::VectorModel<TracePoint>> traceModel_;
    float peak_ = 0.0f;
    bool statusIsError_ = false;
};

} // namespace

int run() {
    // The assets are read before the window opens, so a broken install says which file is
    // missing rather than showing an empty window that will not start. Phase 7's
    // first-run flow is where this becomes something friendlier than a line on stderr.
    std::unique_ptr<engine::LiveTracker> tracker;
    try {
        tracker = std::make_unique<engine::LiveTracker>(weightsPath(), stateSpacePath());
    } catch (const std::exception& e) {
        std::cerr << "takt4: " << e.what() << '\n';
        return 1;
    }

    WindowController controller(*tracker);
    controller.run();
    // The window has gone; stop the audio before the tracker goes with it.
    tracker->stop();
    return 0;
}

} // namespace takt4::ui
