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
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
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

/// "note 36 ch 10" — the control half of a binding, without the action.
///
/// The row already says which action, in the picker right beside this text, so repeating
/// it would spend the width twice. One spelling for both a binding and a loose event, so
/// "not bound - last seen note 36 ch 10" reads against the bound ones above it.
std::string describeControl(const control::MidiBinding& binding) {
    const std::string whole = control::formatMidiBinding(binding);
    return whole.substr(0, whole.find(" ->"));
}

std::string describeControl(const control::MidiEvent& event) {
    return describeControl(
        control::MidiBinding{event.kind, event.channel, event.number, control::ControlAction::Tap});
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

/// A port number a person typed. -1 for anything that is not one, which `setOscControlPort`
/// reports rather than acting on: a field that silently ignored "70o1" would leave an
/// operator pointing a control surface at a port nobody is listening on.
int readPort(const std::string& text) {
    std::string_view view(text);
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
        view.remove_prefix(1);
    }
    while (!view.empty() && (view.back() == ' ' || view.back() == '\t')) {
        view.remove_suffix(1);
    }
    if (view.empty()) {
        return -1;
    }
    int value = 0;
    const char* const begin = view.data();
    const char* const end = begin + view.size();
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return -1;
    }
    return value;
}

/// §5.7's listening socket, from the machine half — a port on this box, never a preset's.
///
/// `enabled` is carried across but the socket is not opened here: the constructor builds
/// this and `setOscControlEnabled` starts it, after the window exists, so a port that is
/// already taken reports on a status line instead of throwing out of a constructor.
control::OscControl::Config oscControlConfig(const settings::Settings& settings) {
    control::OscControl::Config config;
    config.enabled = settings.machine.oscControlEnabled;
    config.port = settings.machine.oscControlPort;
    config.localOnly = settings.machine.oscControlLocalOnly;
    // One namespace in both directions: §5.6 publishes `<prefix>/bpm` and §5.7 listens on
    // `<prefix>/ctl/…`, and an operator who has learned one has learned the other. The
    // prefix is the portable half's — it describes the app, not the box — while whether to
    // listen at all is machine-local.
    if (!settings.preset.oscPrefix.empty()) {
        config.prefix = settings.preset.oscPrefix;
    }
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
      runner_(tracker.engine(), transportConfig(settings, tracker.engine().tempoOptions())),
      // Both built disabled; `setMidiControlPort` and `setOscControlEnabled` are what open
      // a port, so a machine with a controller plugged in is not listened to — and no
      // socket is bound — until somebody says to. The runner is each one's second
      // destination: §5.7's `panic` and `rule/<id>/enable` are questions for the rules,
      // which live on the output thread behind that queue.
      control_(tracker.engine(), control::MidiControl::Config{}, &runner_),
      oscControl_(tracker.engine(), oscControlConfig(settings), &runner_),
      // §5.9's editor, built with the window and shown on demand. It is given the rules
      // straight from the preset rather than through `setRules`, because the runner has
      // not been told about them yet — the constructor body does that below, once.
      editor_(runner_, settings.preset.rules) {
    // §4.3's stamp is taken on the audio thread, so the clock has to be installed before a
    // stream is opened. Handing it to the tracker rather than to the engine is what makes
    // that ordering `LiveTracker::start`'s business instead of this class's.
    tracker_.setHostTimeSource(&runner_.hostTimeClock());
    midiPorts_ = output::listMidiOutputPorts();
    midiInputPorts_ = output::listMidiInputPorts();

    window_->set_trace(traceModel_);

    window_->on_device_picked([this](int index) { pickDevice(index); });
    window_->on_channel_picked([this](int index) { pickChannel(index); });
    window_->on_toggle_run([this] { toggleRun(); });

    window_->on_halve([this] { halve(); });
    window_->on_redouble([this] { redouble(); });
    window_->on_tap([this] { tap(); });
    window_->on_snap_downbeat([this] { snapDownbeat(); });
    window_->on_pin_changed([this](bool pinned) { setPinned(pinned); });
    window_->on_fold_on_changed([this](bool on) { setFoldEnabled(on); });
    window_->on_fold_min_changed([this](float bpm) { setFoldMin(static_cast<double>(bpm)); });
    window_->on_fold_max_changed([this](float bpm) { setFoldMax(static_cast<double>(bpm)); });
    window_->on_latency_changed([this](float ms) { setLatencyMs(static_cast<double>(ms)); });

    window_->on_link_toggled([this](bool on) { setLinkEnabled(on); });
    window_->on_osc_targets_edited(
        [this](const slint::SharedString& text) { setOscTargets(std::string(text)); });
    window_->on_midi_port_picked(
        [this](const slint::SharedString& name) { setMidiPort(std::string(name)); });

    window_->on_midi_in_picked(
        [this](const slint::SharedString& name) { setMidiControlPort(std::string(name)); });
    window_->on_learn_action_picked([this](int index) { pickLearnAction(index); });
    window_->on_learn_clicked([this] { toggleLearn(); });
    window_->on_forget_clicked([this] { forgetLearned(); });

    window_->on_osc_control_toggled([this](bool on) { setOscControlEnabled(on); });
    window_->on_osc_control_port_edited([this](const slint::SharedString& text) {
        setOscControlPort(readPort(std::string(text)));
    });
    window_->on_osc_control_network_toggled([this](bool on) { setOscControlNetwork(on); });

    window_->on_rules_clicked([this] { openEditor(); });
    window_->on_panic_clicked([this] { togglePanic(); });

    // The editor owns the editing and this owns the file, so a change there comes back
    // here rather than the editor knowing where settings live.
    editor_.setRulesChanged(
        [this](const std::vector<trigger::Rule::Config>& rules) { rules_ = rules; });

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

    auto inputs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    inputs->push_back(shared("")); // "none", as above
    for (const std::string& port : midiInputPorts_) {
        inputs->push_back(shared(port));
    }
    window_->set_midi_in_ports(inputs);

    // Everything a gesture can bind on its own. §5.7's `rule/<id>/enable` is the one that
    // cannot: pressing a pad says which button, never which rule, and this window has
    // nowhere to ask the second question. §5.9's rule editor is where that binding gets
    // armed, from the card that already names the rule — `MidiControl::learn` takes a whole
    // target for exactly that.
    auto actions = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const control::ControlAction action : control::kControlActions) {
        if (control::takesRuleId(action)) {
            continue;
        }
        learnActions_.push_back(action);
        actions->push_back(shared(std::string(control::labelOf(action))));
    }
    window_->set_learn_actions(actions);

    // Whatever was learned last time, restored. A line this build cannot read is skipped
    // rather than refused: `settings` keeps the text it was given, so a file written by a
    // later build with an action this one does not have still loads the rest.
    std::vector<control::MidiBinding> bindings;
    for (const std::string& text : settings.machine.midiBindings) {
        if (const std::optional<control::MidiBinding> binding = control::parseMidiBinding(text)) {
            bindings.push_back(*binding);
        }
    }
    control_.setBindings(std::move(bindings));

    refreshDevices(settings.machine);
    // After the pickers, so a port that has gone missing since the last run reports on a
    // status line the window already has rather than during construction.
    if (!settings.machine.midiClockPort.empty()) {
        setMidiPort(settings.machine.midiClockPort);
    }
    if (!settings.machine.midiControlPort.empty()) {
        setMidiControlPort(settings.machine.midiControlPort);
    }
    // §5.8's rules from the preset half, handed to the output thread. Before the OSC
    // socket, so a control surface that comes up listening cannot enable a rule that has
    // not been loaded yet.
    if (!settings.preset.rules.empty()) {
        setRules(settings.preset.rules);
    }
    if (settings.machine.oscControlEnabled) {
        // The socket is bound here rather than in the member initialiser, for the same
        // reason the MIDI port is: a port another application has taken throws, and by now
        // there is a status line to say so on.
        //
        // `setOscControlEnabled` reads `running()`, which is false, so this really does
        // start it rather than seeing the config flag and returning.
        setOscControlEnabled(true);
    }
    publishStopped();
    publishOutputs();
    publishControl();

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

void WindowController::setMidiControlPort(const std::string& name) {
    // The bindings survive this: `setPort` keeps them, because an operator moving from
    // one controller to another is not asking to forget what they learned.
    control_.setPort(name);
    if (!name.empty()) {
        try {
            control_.start();
            setStatus("Control input on " + control_.portName() +
                          ". Pick an action, press LEARN, then press the control.",
                      false);
        } catch (const std::exception& e) {
            // The port list is what the machine offered when the window opened; a
            // controller unplugged since then lands here, and saying so is the whole
            // reason `start()` throws rather than quietly listening to nothing.
            setStatus(e.what(), true);
        }
    }
    publishControl();
}

void WindowController::openEditor() {
    editor_.show();
}

void WindowController::togglePanic() {
    runner_.panic(!runner_.panicked());
    publishTriggers();
}

void WindowController::setRules(std::vector<trigger::Rule::Config> rules) {
    rules_ = std::move(rules);
    // The editor's copy too, or it would go on showing the set it was built with — and the
    // next edit there would post that stale set back over this one.
    editor_.setRules(rules_);
    // The whole set, every time. A rule is small and the set is short, so there is no
    // reason for a finer command — and replacing wholesale is what a preset load does, so
    // the editor and the loader take one road rather than two.
    runner_.post(output::OutputCommand::rules(rules_));

    // §5.8's other policy: an invalid rule is *held* and refuses to fire, rather than being
    // refused on the way in. Nothing else would tell an operator, so this does — and it
    // counts rather than naming one, because a preset arriving with four broken rules
    // should not report only the first.
    std::size_t broken = 0;
    for (const trigger::Rule::Config& config : rules_) {
        if (!trigger::Rule(config).valid()) {
            ++broken;
        }
    }
    if (broken > 0) {
        setStatus(std::to_string(broken) + (broken == 1 ? " rule will not fire until it is fixed."
                                                        : " rules will not fire until fixed."),
                  true);
    }
}

void WindowController::setOscControlEnabled(bool on) {
    if (on == oscControl_.running()) {
        return;
    }
    if (!on) {
        oscControl_.stop();
        setStatus("OSC control off.", false);
        publishControl();
        return;
    }

    // `start()` reads `config().enabled`, so the flag has to be set before it is called —
    // and a port that is taken throws out of `start()` rather than out of the thread, which
    // is the whole reason it throws at all.
    control::OscControl::Config config = oscControl_.config();
    config.enabled = true;
    oscControl_.setConfig(config);
    try {
        oscControl_.start();
        setStatus("OSC control listening on " + std::to_string(oscControl_.port()) +
                      (config.localOnly ? " (this machine only)." : " (any address)."),
                  false);
    } catch (const std::exception& e) {
        // A port another application already has. Saying so is the point: a control
        // surface that silently does nothing is worse than one that will not start.
        config.enabled = false;
        oscControl_.setConfig(config);
        setStatus(std::string("OSC control: ") + e.what(), true);
    }
    publishControl();
}

void WindowController::setOscControlPort(int port) {
    if (port < 0 || port > 65535) {
        setStatus("A port is 0 to 65535; 0 asks for any free one.", true);
        publishControl();
        return;
    }
    control::OscControl::Config config = oscControl_.config();
    if (config.port == static_cast<std::uint16_t>(port)) {
        return;
    }
    config.port = static_cast<std::uint16_t>(port);

    // A socket is bound at `start()`, so changing the port means going round again — but
    // only if it was listening. Editing the number while it is off is just editing a
    // number, and must not open a socket nobody asked for.
    const bool wasRunning = oscControl_.running();
    oscControl_.stop();
    config.enabled = false;
    oscControl_.setConfig(config);
    if (wasRunning) {
        setOscControlEnabled(true);
    } else {
        publishControl();
    }
}

void WindowController::setOscControlNetwork(bool allowNetwork) {
    control::OscControl::Config config = oscControl_.config();
    if (config.localOnly == !allowNetwork) {
        return;
    }
    config.localOnly = !allowNetwork;
    const bool wasRunning = oscControl_.running();
    oscControl_.stop();
    config.enabled = false;
    oscControl_.setConfig(config);
    if (wasRunning) {
        setOscControlEnabled(true);
    } else {
        publishControl();
    }
}

void WindowController::pickLearnAction(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= learnActions_.size()) {
        return;
    }
    learnAction_ = index;
    // Changing what LEARN would bind while it is armed is a different intention, so the
    // arming does not carry over to it.
    control_.cancelLearn();
    publishControl();
}

void WindowController::toggleLearn() {
    if (control_.learning()) {
        control_.cancelLearn();
    } else {
        control_.learn(learnActions_[static_cast<std::size_t>(learnAction_)]);
    }
    publishControl();
}

void WindowController::forgetLearned() {
    const control::ControlAction action = learnActions_[static_cast<std::size_t>(learnAction_)];
    control_.cancelLearn();
    (void)control_.forget(action);
    publishControl();
}

void WindowController::publishControl() {
    // Two independent surfaces, published independently. **Not one function with two
    // halves**: the MIDI half returns early when no port is open, and an OSC half written
    // below that return was silently never published — caught by
    // `tests/ui/window_test.cpp`'s "opens only when asked", which found the port field
    // empty on a window whose socket was bound.
    publishMidiControl();
    publishOscControl();
}

void WindowController::publishMidiControl() {
    window_->set_control_on(control_.running());
    window_->set_learning(control_.learning().has_value());
    window_->set_learn_action_index(learnAction_);

    if (!control_.running()) {
        window_->set_control_reading(shared(
            midiInputPorts_.empty() ? "no MIDI inputs on this machine" : "off — pick a port"));
        return;
    }
    if (control_.learning()) {
        window_->set_control_reading(shared("waiting for a control..."));
        return;
    }

    // What the selected action is bound to. More than one control can be bound to one
    // action — two pads for one job is a reasonable thing to want — so they are all shown.
    const control::ControlAction action = learnActions_[static_cast<std::size_t>(learnAction_)];
    std::string text;
    for (const control::MidiBinding& binding : control_.bindings()) {
        if (binding.target.action != action) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += describeControl(binding);
    }
    if (text.empty()) {
        // Nothing bound. Say what did arrive instead, if anything has: a controller on a
        // channel nothing is listening to looks exactly like a broken cable otherwise.
        const std::optional<control::MidiEvent> last = control_.lastEvent();
        text = last ? "not bound - last seen " + describeControl(*last) : std::string("not bound");
    }
    window_->set_control_reading(shared(text));
}

void WindowController::publishTriggers() {
    int active = 0;
    for (const trigger::Rule::Config& config : rules_) {
        // What "active" means to an operator: it would fire if its moment came. A rule that
        // is switched off and one that cannot fire are both not going to, and counting them
        // as active would make the row lie in the direction that matters.
        if (config.enabled && trigger::Rule(config).valid()) {
            ++active;
        }
    }
    window_->set_rules_active(active);
    window_->set_rules_total(static_cast<int>(rules_.size()));
    window_->set_panicked(runner_.panicked());
}

void WindowController::publishOscControl() {
    const control::OscControl::Config& config = oscControl_.config();
    const bool listening = oscControl_.running();
    window_->set_osc_control_on(listening);
    window_->set_osc_control_network(!config.localOnly);
    // The port bound while it is listening, and the one asked for while it is not. With 0
    // meaning "any free one" those differ, and only the bound one is a number an operator
    // can point a Stream Deck at.
    window_->set_osc_control_port(
        shared(std::to_string(listening ? oscControl_.port() : config.port)));

    if (!listening) {
        window_->set_osc_control_reading(shared("off"));
        return;
    }
    // Two different questions, and only one of them is live at a time. Before anything has
    // arrived the operator needs the address to aim at; once packets are landing they need
    // to know what landed, and the address has answered itself.
    const std::uint64_t handled = oscControl_.handled();
    const std::uint64_t ignored = oscControl_.ignored();
    if (handled == 0 && ignored == 0) {
        window_->set_osc_control_reading(shared(config.prefix + "/ctl/...  nothing yet"));
        return;
    }
    std::string text = oscControl_.lastMessage();
    if (!text.empty()) {
        text += "  ";
    }
    // Counted separately, because "arriving but not understood" is a different fault from
    // "not arriving" and they need different fixes — a wrong prefix against a wrong
    // address. Nothing else on screen tells the two apart.
    text += std::to_string(handled) + " acted, " + std::to_string(ignored) + " ignored";
    window_->set_osc_control_reading(shared(text));
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
    if (!tracker_.engine().post(engine::Command::snapDownbeat())) {
        return; // queue full; nothing is coming, so do not light the button
    }
    // The snap's correction reaches the outputs on the next beat the tracker calls, which
    // at 70 BPM is the best part of a second away — even when the bar dots move at once,
    // which they do whenever the press named the beat just gone. Remember the beat count
    // now so the redraw can tell when it has landed, and light the button until then: a
    // press between beats is otherwise entirely invisible, and a bar phase shifting is hard
    // to see moving even once it does.
    snapAwaitingBeat_ = tracker_.engine().state().beats;
    window_->set_snap_pending(true);
}

void WindowController::publishSnap() {
    if (!snapAwaitingBeat_) {
        return;
    }
    // A beat arriving is what says the snap landed. One could slip in between the post and
    // the inference thread draining it — about two milliseconds against a beat period of
    // hundreds — and clear this a beat early; that is a light going out slightly too soon,
    // which is not worth a second counter through the engine to prevent.
    if (tracker_.engine().state().beats != *snapAwaitingBeat_) {
        snapAwaitingBeat_.reset();
        window_->set_snap_pending(false);
    }
}

void WindowController::setPinned(bool pinned) {
    if (!tracker_.engine().post(engine::Command::setLockPinned(pinned))) {
        // The queue is full, which takes 3200 posts a second. Nothing was sent, so nothing
        // is in flight and the button goes back to telling the truth.
        pinPosted_.reset();
        return;
    }
    pinPosted_ = pinned;
    pinSettling_ = 0;
    // Optimistically, so the next press reads this rather than the state the engine has
    // not been given a frame to update yet.
    window_->set_pinned(pinned);
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

    // §5.7's control surface, which is machine-local for the same reason and more so: what
    // was learned describes the box of buttons on this desk.
    out.machine.midiControlPort = control_.config().port;
    for (const control::MidiBinding& binding : control_.bindings()) {
        out.machine.midiBindings.push_back(control::formatMidiBinding(binding));
    }

    // §5.7's other surface. `running()` rather than `config().enabled`, so a port that was
    // taken at startup is remembered as *off* — the operator saw the error and did not get
    // a listener, and a file that claims otherwise would fail the same way every launch.
    // The port asked for, not `port()`: with 0 meaning "any free one", saving what the
    // platform happened to hand out would silently pin next launch to it.
    out.machine.oscControlEnabled = oscControl_.running();
    out.machine.oscControlPort = oscControl_.config().port;
    out.machine.oscControlLocalOnly = oscControl_.config().localOnly;

    // The tracker's own, not this window's copy: a tap moves the fold window and the
    // window is only ever showing what the engine has (§7 deviation 8).
    out.preset.tempo = tracker_.engine().tempoOptions();
    out.preset.link = transports.linkEnabled();
    out.preset.oscTargets = transports.oscTargets();
    out.preset.oscPrefix = transports.oscPrefix();
    // This window's copy, not the runner's: the runner's belong to the output thread and
    // reading them while it runs is what `rules()` explains is unsafe.
    out.preset.rules = rules_;
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
    // A set of taps does not span a stop, and the button must not go on counting. Nor
    // does a pin: `BeatEngine::start` reseeds the tracker, which lets go of it, so a pin
    // still in flight here would be showing an intention the next run will not honour.
    taps_.reset();
    pinPosted_.reset();
    // A snap that never landed does not survive a stop either; the next run reseeds the
    // tracker, so there is no beat coming that it was waiting for.
    snapAwaitingBeat_.reset();
    window_->set_snap_pending(false);
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

void WindowController::publishPin() {
    const bool live = tracker_.engine().state().pinned;
    if (pinPosted_) {
        if (live != *pinPosted_) {
            // The race `publishOptions` guards, for the same reason and on the same
            // deadline: a redraw landing between the post and the inference thread
            // draining it would read the old value back and un-light the button under the
            // operator's finger.
            if (!tracker_.running() || ++pinSettling_ <= kSettleRedraws) {
                window_->set_pinned(*pinPosted_);
                return;
            }
        }
        pinPosted_.reset();
    }
    window_->set_pinned(live);
}

void WindowController::setStatus(const std::string& text, bool error) {
    statusIsError_ = error;
    window_->set_status(shared(text));
    window_->set_status_is_error(error);
}

void WindowController::tick() {
    ++ticks_;
    // Before the early return: a MIDI message arrives on RtMidi's thread, so what it
    // changed — a learn that took, a control that was seen — only reaches the window on a
    // redraw, and binding buttons is something an operator does *before* pressing Start.
    publishControl();
    // Above the early return with it, and for a related reason: what these two watch is
    // the engine's own state, which moves whether or not a device is open. A test drives
    // the engine directly without one, and holding the button lit forever there would be
    // the window lying about a snap that had in fact landed.
    publishSnap();
    // And the rules, for the same reason twice over: they fire on the output thread, and
    // the editor is where an operator watches them. Both above the early return — a rule
    // can be built and tested with no device open, which is exactly how one gets built.
    publishTriggers();
    editor_.tick();
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
    // After it: publishTempoState shows what the engine has, and this is the one property
    // the window may legitimately be showing ahead of it.
    publishPin();
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
