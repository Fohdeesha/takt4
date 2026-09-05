#include "ui/window_controller.hpp"

#include "core/audio/channel_picker.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/control.hpp"
#include "core/output/midi_ports.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

namespace takt4::ui {
namespace {

using Options = tracking::TempoTracker::Options;

slint::SharedString shared(const std::string& text) {
    return slint::SharedString(text);
}

/// Whether the engine has taken the settings the window sent it.
///
/// Only the four fields the window can change. Comparing the rest would leave a control
/// frozen over a difference this window did not cause and cannot fix — and exact equality
/// is the right test for all four, because what comes back is the value that was posted,
/// stored and read back, never a number arrived at by arithmetic.
bool sameSettings(const Options& a, const Options& b) noexcept {
    return a.octaveFold == b.octaveFold && a.minBpm == b.minBpm && a.maxBpm == b.maxBpm &&
           a.latencyOffsetSeconds == b.latencyOffsetSeconds;
}

/// What the last run was sending, as the transports want it.
///
/// The MIDI port comes from the machine half and everything else from the portable half,
/// which is Q7's split: an OSC address travels to another laptop and a port on this box
/// does not. A port that has since been unplugged throws on the way up, so it is left for
/// `setMidiPort` to try once the window exists and can say so.
output::Transports::Config transportConfig(const settings::Settings& settings,
                                           const Options& tempo) {
    output::Transports::Config config;
    config.link = settings.preset.link;
    config.oscPrefix = settings.preset.oscPrefix;
    config.oscTargets = settings.preset.oscTargets;
    config.latencySeconds = tempo.latencyOffsetSeconds;
    return config;
}

} // namespace

WindowController::WindowController(engine::LiveTracker& tracker)
    : WindowController(tracker, settings::Settings{}) {}

WindowController::WindowController(engine::LiveTracker& tracker, const settings::Settings& settings)
    : tracker_(tracker), window_(MainWindow::create()), trace_(kTraceLength),
      traceModel_(
          std::make_shared<slint::VectorModel<TracePoint>>(std::vector<TracePoint>(kTraceLength))),
      // Whatever the last run was sending, switched back on. With no settings that is
      // nothing, which is what an app nobody has configured should send.
      runner_(tracker.engine(), transportConfig(settings, tracker.engine().tempoOptions())) {
    // §4.3's stamp is taken on the audio thread, so the clock has to be installed before a
    // stream is opened. Handing it to the tracker rather than to the engine is what makes
    // that ordering `LiveTracker::start`'s business instead of this class's.
    tracker_.setHostTimeSource(&runner_.hostTimeClock());
    midiPorts_ = output::listMidiOutputPorts();

    window_->set_trace(traceModel_);

    window_->on_device_picked([this](int index) { pickDevice(index); });
    window_->on_channel_picked([this](int index) { pickChannel(index); });
    window_->on_toggle_run([this] { toggleRun(); });

    window_->on_halve([this] { halve(); });
    window_->on_redouble([this] { redouble(); });
    window_->on_tap([this] { tap(); });
    window_->on_snap_downbeat([this] { snapDownbeat(); });
    window_->on_fold_on_changed([this](bool on) { setFoldEnabled(on); });
    window_->on_fold_min_changed([this](float bpm) { setFoldMin(static_cast<double>(bpm)); });
    window_->on_fold_max_changed([this](float bpm) { setFoldMax(static_cast<double>(bpm)); });
    window_->on_latency_changed([this](float ms) { setLatencyMs(static_cast<double>(ms)); });

    window_->on_link_toggled([this](bool on) { setLinkEnabled(on); });
    window_->on_osc_targets_edited(
        [this](const slint::SharedString& text) { setOscTargets(std::string(text)); });
    window_->on_midi_port_picked(
        [this](const slint::SharedString& name) { setMidiPort(std::string(name)); });

    publishControlLimits(*window_);
    window_->set_tap_needs(static_cast<int>(taps_.options().needTaps));

    auto ports = std::make_shared<slint::VectorModel<slint::SharedString>>();
    // An empty first entry is "none", so turning MIDI clock off is a choice in the same
    // list rather than a second control.
    ports->push_back(shared(""));
    for (const std::string& port : midiPorts_) {
        ports->push_back(shared(port));
    }
    window_->set_midi_ports(ports);

    refreshDevices(settings.machine);
    // After the pickers, so a port that has gone missing since the last run reports on a
    // status line the window already has rather than during construction.
    if (!settings.machine.midiClockPort.empty()) {
        setMidiPort(settings.machine.midiClockPort);
    }
    publishStopped();
    publishOutputs();

    timer_.start(slint::TimerMode::Repeated, kRedrawInterval, [this] { tick(); });
}

void WindowController::run() {
    window_->run();
}

void WindowController::refreshDevices(const settings::MachineSettings& remembered) {
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

    // What the last run was on, if it is still plugged in. By name and host API rather
    // than by index, because an index means something different the moment anything else
    // is connected — and restoring the wrong interface is worse than restoring none.
    std::size_t chosen = devices_.size();
    if (!remembered.deviceName.empty()) {
        for (std::size_t i = 0; i < devices_.size(); ++i) {
            if (devices_[i].name == remembered.deviceName &&
                devices_[i].hostApiName == remembered.hostApiName) {
                chosen = i;
                break;
            }
        }
    }

    const bool restored = chosen != devices_.size();
    if (!restored) {
        // Nothing remembered, or it is not here any more. The most useful device to land
        // on: a real input before a loopback of the speakers (Q3 offers loopback as a way
        // round a busy interface, not as the normal path), then a host API that can pick
        // channels natively, then the machine's own default input, then the most inputs.
        const auto rank = [](const audio::InputDevice& device) {
            return std::make_tuple(!device.isLoopback,
                                   audio::hasNativeChannelSelection(device.hostApi),
                                   device.isDefaultInput, device.maxInputChannels);
        };
        chosen = 0;
        for (std::size_t i = 1; i < devices_.size(); ++i) {
            if (rank(devices_[i]) > rank(devices_[chosen])) {
                chosen = i;
            }
        }
    }

    window_->set_device_index(static_cast<int>(chosen));
    pickDevice(static_cast<int>(chosen));

    // The channel only after the device, because picking a device resets it to the first
    // input. And only when the device it was remembered *for* is the one we landed on: a
    // channel number means nothing on a different interface, and "input 6" of whatever
    // happened to be second in the list is exactly the wrong kind of restored setting.
    if (restored && remembered.channel > 0 &&
        remembered.channel < devices_[static_cast<std::size_t>(device_)].maxInputChannels) {
        pickChannel(remembered.channel);
        window_->set_channel_index(remembered.channel);
    }
}

void WindowController::pickDevice(int index) {
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

void WindowController::toggleRun() {
    if (tracker_.running()) {
        // The tracker first: it tracks the hops still in flight on the way down, and those
        // can call a last beat that the runner should still send.
        tracker_.stop();
        runner_.stop();
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
        // HANDOFF Q3 and R2: most ASIO drivers are single-client, and a busy interface is
        // the exact situation takt4 exists for. A PortAudio error code is no use to anyone
        // standing at a laptop, so say what it means and what the ways round it are.
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
    // After the tracker, so nothing is sent for a run that failed to open.
    runner_.start();
    window_->set_running(true);
    publishOpenStream();
}

void WindowController::setLinkEnabled(bool on) {
    runner_.post(output::OutputCommand::linkEnabled(on));
    publishOutputs();
}

void WindowController::setOscTargets(const std::string& text) {
    // One host:port per line or per comma — the window offers a single line, so a comma
    // is how more than one fits in it. A part that will not parse is named on the status
    // line and the rest are still applied: an operator halfway through typing an address
    // must not lose the ones that already worked.
    std::string separated = text;
    std::replace(separated.begin(), separated.end(), ',', '\n');
    std::vector<output::Transports::OscTarget> targets;
    std::string bad;
    std::istringstream lines(separated);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) {
            continue;
        }
        const std::size_t end = line.find_last_not_of(" \t\r");
        const std::string trimmed = line.substr(begin, end - begin + 1);
        const std::size_t colon = trimmed.rfind(':');
        int port = 0;
        if (colon != std::string::npos && colon + 1 < trimmed.size()) {
            const std::string digits = trimmed.substr(colon + 1);
            port = std::all_of(digits.begin(), digits.end(),
                               [](unsigned char c) { return std::isdigit(c) != 0; })
                       ? std::atoi(digits.c_str())
                       : 0;
        }
        if (colon == std::string::npos || colon == 0 || port <= 0 || port > 65535) {
            if (bad.empty()) {
                bad = trimmed;
            }
            continue;
        }
        targets.emplace_back(trimmed.substr(0, colon), static_cast<std::uint16_t>(port));
    }
    runner_.post(output::OutputCommand::oscTargets(std::move(targets)));
    if (!bad.empty()) {
        setStatus("OSC: \"" + bad + "\" is not host:port, so it was left out.", true);
    } else if (statusIsError_) {
        setStatus("Pick an input and press Start.", false);
    }
    publishOutputs();
}

void WindowController::setMidiPort(const std::string& name) {
    runner_.post(output::OutputCommand::midiClockPort(
        name.empty() ? std::optional<std::string>{} : std::optional<std::string>{name}));
    const std::string error = runner_.lastError();
    if (!error.empty()) {
        setStatus("MIDI clock: " + error, true);
    }
    publishOutputs();
}

double WindowController::nowSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

void WindowController::halve() {
    (void)tracker_.engine().post(engine::Command::halve());
}

void WindowController::redouble() {
    (void)tracker_.engine().post(engine::Command::redouble());
}

void WindowController::snapDownbeat() {
    (void)tracker_.engine().post(engine::Command::snapDownbeat());
}

void WindowController::tap() {
    tap(nowSeconds());
}

void WindowController::tap(double seconds) {
    const std::optional<double> tapped = taps_.tap(seconds);
    lastTapSeconds_ = seconds;
    if (tapped) {
        // A seed, not an override: the fold window moves onto the tapped octave and the
        // tracker keeps tracking (§7's locked decisions). It moves `minBpm`/`maxBpm` under
        // this window, which is why nothing here holds a copy of them — and why a settings
        // change in flight is dropped, since the tap has just superseded it.
        (void)tracker_.engine().post(engine::Command::seedTempo(*tapped));
        posted_.reset();
    }
    publishTaps();
}

void WindowController::setFoldEnabled(bool on) {
    Options options = settings();
    options.octaveFold = on;
    postOptions(options);
}

void WindowController::setFoldMin(double bpm) {
    Options options = settings();
    // Clamped against the other end rather than against the constant alone, so the two
    // cannot cross however the value arrived — a slider is bounded by its own markup, but
    // this is also the way in for a test and, later, for an inbound OSC message (§5.7).
    options.minBpm =
        std::clamp(bpm, kFoldFloorBpm, std::max(kFoldFloorBpm, options.maxBpm - kFoldLeastSpanBpm));
    postOptions(options);
}

void WindowController::setFoldMax(double bpm) {
    Options options = settings();
    options.maxBpm = std::clamp(bpm, std::min(kFoldCeilingBpm, options.minBpm + kFoldLeastSpanBpm),
                                kFoldCeilingBpm);
    postOptions(options);
}

void WindowController::setLatencyMs(double milliseconds) {
    Options options = settings();
    options.latencyOffsetSeconds =
        std::clamp(milliseconds, -kLatencyLimitMs, kLatencyLimitMs) / 1000.0;
    postOptions(options);
}

tracking::TempoTracker::Options WindowController::settings() const {
    // §7 deviation 8 says to edit from what the engine has and never from a copy kept
    // since startup, because a tap moves the fold window underneath one. `posted_` is not
    // that copy: it is a change made moments ago and not yet applied, it is cleared the
    // moment the engine is seen to have it or a tap supersedes it, and editing from it is
    // what stops two quick drags from undoing each other.
    return posted_ ? *posted_ : tracker_.engine().tempoOptions();
}

void WindowController::postOptions(const Options& options) {
    if (!tracker_.engine().post(engine::Command::setTempoOptions(options))) {
        // The queue is full, which takes 3200 posts a second. Nothing was sent, so
        // nothing is in flight and the window goes back to telling the truth.
        posted_.reset();
        return;
    }
    posted_ = options;
    settling_ = 0;
    publishTempoOptions(*window_, options);
}

void WindowController::publishTaps() {
    window_->set_tap_count(static_cast<int>(taps_.taps()));
}

settings::Settings WindowController::currentSettings() const {
    settings::Settings out;
    if (device_ >= 0 && static_cast<std::size_t>(device_) < devices_.size()) {
        const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
        out.machine.deviceName = device.name;
        out.machine.hostApiName = device.hostApiName;
        out.machine.channel = channel_;
    }
    const output::Transports& transports = runner_.transports();
    out.machine.midiClockPort = transports.midiClockPort().value_or(std::string{});

    // The tracker's own, not this window's copy: a tap moves the fold window and the
    // window is only ever showing what the engine has (§7 deviation 8).
    out.preset.tempo = tracker_.engine().tempoOptions();
    out.preset.link = transports.linkEnabled();
    out.preset.oscTargets = transports.oscTargets();
    out.preset.oscPrefix = transports.oscPrefix();
    return out;
}

void WindowController::publishOutputs() {
    const output::Transports& transports = runner_.transports();
    window_->set_link_on(transports.linkEnabled());
    window_->set_link_peers(static_cast<int>(transports.link().numPeers()));

    std::string osc;
    for (const auto& [host, port] : transports.oscTargets()) {
        if (!osc.empty()) {
            osc += '\n';
        }
        osc += host + ":" + std::to_string(port);
    }
    window_->set_osc_targets(shared(osc));
    window_->set_osc_on(!transports.oscTargets().empty());

    window_->set_midi_port(shared(transports.midiClockPort().value_or(std::string{})));
    window_->set_midi_on(transports.midiClock() != nullptr);
    window_->set_beats_sent(static_cast<int>(transports.beats()));
}

void WindowController::publishStopped() {
    window_->set_running(false);
    // A set of taps does not span a stop, and the button must not go on counting.
    taps_.reset();
    publishTaps();
    publishIdleReadouts(*window_);
    publishOptions();
    if (!devices_.empty() && !statusIsError_) {
        setStatus("takt4 " + buildInfo().version + " — pick an input and press Start.", false);
    }
}

void WindowController::publishOpenStream() {
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

void WindowController::publishOptions() {
    const Options live = tracker_.engine().tempoOptions();
    if (posted_) {
        if (!sameSettings(live, *posted_)) {
            // A change is in flight. Leave the controls showing it: the inference thread
            // applies a posted command within about two milliseconds, but a redraw landing
            // inside that window would read the old value back and snap the slider out
            // from under the operator's finger.
            //
            // The deadline only runs while the engine does, because only then is anything
            // draining the queue. A change made to a stopped tracker waits there until the
            // next start — `BeatEngine::start` applies it before its first frame — so
            // showing it rather than counting it out is the window telling the truth.
            if (!tracker_.running() || ++settling_ <= kSettleRedraws) {
                return;
            }
        }
        // Either the engine has it, or it never took it — a `SetTempoOptions` superseded
        // by a tap, say. Both end the same way: the window stops showing what it sent and
        // goes back to showing what the tracker has.
        posted_.reset();
    }
    publishTempoOptions(*window_, live);
}

void WindowController::setStatus(const std::string& text, bool error) {
    statusIsError_ = error;
    window_->set_status(shared(text));
    window_->set_status_is_error(error);
}

void WindowController::tick() {
    ++ticks_;
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

    // The beats are not drained here: `runner_` is the single consumer of that ring, and
    // two of them would each take half of them. The frame ring above is a different ring
    // with a different consumer, which is this.

    // A set of taps that has gone quiet is over: `TapTempo` starts a fresh set on the next
    // tap anyway, and until then the button should not claim to be counting one.
    if (taps_.taps() > 0 && nowSeconds() - lastTapSeconds_ > taps_.options().timeoutSeconds) {
        taps_.reset();
        publishTaps();
    }

    publishState();
    publishLevels();
    // Peers come and go, and the beat counter is the only thing on screen that says a
    // transport is really doing something.
    publishOutputs();
}

void WindowController::publishTrace() {
    for (std::size_t i = 0; i < trace_.size(); ++i) {
        traceModel_->set_row_data(i, trace_[i]);
    }
}

void WindowController::publishState() {
    publishTempoState(*window_, tracker_.engine().state());
    publishOptions();
}

void WindowController::publishLevels() {
    float loudest = -1.0f; // negative: no hop arrived this tick
    float peak = 0.0f;
    audio::HopLevel level;
    while (tracker_.popLevel(level)) {
        loudest = std::max(loudest, level.rms);
        peak = std::max(peak, level.peak);
    }
    peak_ = std::max(peak, peak_ * kPeakDecay);
    if (loudest < 0.0f) {
        // Nothing arrived this tick: let the peak fall, leave the reading where it was
        // rather than flashing to silence.
        window_->set_input_peak(peak_);
        return;
    }
    publishInput(*window_, loudest, peak_);
}

} // namespace takt4::ui
