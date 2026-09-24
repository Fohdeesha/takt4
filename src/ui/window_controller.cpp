#include "ui/window_controller.hpp"

#include "core/assets/embedded.hpp"
#include "core/audio/channel_picker.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/control.hpp"
#include "core/io/utf8.hpp"
#include "core/output/midi_ports.hpp"
#include "ui/file_dialog.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h> // ShellExecuteW, which WIN32_LEAN_AND_MEAN leaves out of windows.h
#endif

namespace takt4::ui {
namespace {

using Options = tracking::TempoTracker::Options;

/// Every string this window shows, made safe to show. See `io::validUtf8`: device and port
/// names come from drivers through ANSI APIs and exceptions carry whatever their library
/// wrote, and one byte Slint cannot decode is an abort rather than a garbled character.
slint::SharedString shared(const std::string& text) {
    return slint::SharedString(io::validUtf8(text));
}

/// Whether the engine has taken the settings the window sent it.
///
/// Only the five fields the window can change. Comparing the rest would leave a control
/// frozen over a difference this window did not cause and cannot fix — and exact equality
/// is the right test for all five, because what comes back is the value that was posted,
/// stored and read back, never a number arrived at by arithmetic.
bool sameSettings(const Options& a, const Options& b) noexcept {
    return a.octaveFold == b.octaveFold && a.minBpm == b.minBpm && a.maxBpm == b.maxBpm &&
           a.latencyOffsetSeconds == b.latencyOffsetSeconds &&
           a.keepOctaveShift == b.keepOctaveShift;
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

/// The preset's OSC prefix if `OscPublisher` will take it, and the default if not.
///
/// `settings::fromJson` already refuses a bad one, so this only matters for settings built
/// some other way — but the publisher throws from inside the runner's constructor, which runs
/// inside this class's, and a throw there is an application that does not open.
std::string usablePrefix(const settings::Settings& settings) {
    return output::isValidOscPrefix(settings.preset.oscPrefix) ? settings.preset.oscPrefix
                                                               : settings::Preset{}.oscPrefix;
}

/// What the last run was sending, as the transports want it **at construction** — which is
/// everything that cannot fail.
///
/// **Not the outputs, and not the MIDI clock port.** Either can name something that is not
/// here today: a USB MIDI interface left at home, a media server whose hostname does not
/// resolve yet because the network is still coming up. `Transports` throws for those, from
/// inside `OutputRunner`'s constructor, from inside this class's member initialisers — and
/// nothing above that caught it, so takt4 died within seconds of every launch with no window
/// and no message (the audit's C1, reproduced with the Release build). The only way out was to
/// hand-edit `settings.json`. So both are applied once the window exists, through the same
/// `post` an operator's edit takes, and a failure lands on the status line like any other.
output::Transports::Config transportConfig(const settings::Settings& settings,
                                           const Options& tempo) {
    output::Transports::Config config;
    config.link = settings.preset.link;
    config.oscPrefix = usablePrefix(settings);
    config.patch = settings.preset.fixtures;
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

/// `TAKT4_TICK_PROBE`, or empty. See `WindowController::writeTickProbe`.
std::string environmentPath(const char* name) {
#if defined(_MSC_VER)
    // Not std::getenv: MSVC deprecates it, and the buffer it returns is not ours.
    std::size_t size = 0;
    if (getenv_s(&size, nullptr, 0, name) != 0 || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (getenv_s(&size, value.data(), size, name) != 0) {
        return {};
    }
    value.resize(size == 0 ? 0 : size - 1); // getenv_s counts the terminator
    return value;
#else
    const char* const value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return text;
}

/// One target's worth of text at a time, from a field that may hold several.
///
/// A row's address box holds one address — that is the point of there being rows — but a
/// line pasted from the field this list replaced holds the whole rig, and `setOscTargets`
/// still takes one. Both end up here, and both end up as rows.
std::vector<std::string_view> splitTargets(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = text.find_first_of(",\n", at);
        const std::string_view part = trim(
            text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (next == std::string_view::npos) {
            break;
        }
        at = next + 1;
    }
    return parts;
}

/// What a row's port box holds when the operator has not said otherwise.
///
/// Not `OutputTarget::port`'s own 7000, which is Resolume's: this is the number the *box* is
/// pre-filled with when [+ ADD OUTPUT] makes a row, and 9000 is what the person who uses this
/// asked for. A prefilled port is worth having at all because it is the half of an OSC
/// destination that has a conventional answer, where the host does not.
constexpr std::uint16_t kNewTargetPort = 9000;

/// An outage's pacing (see `WindowController::superviseInput`). The first try is soon: a
/// driver that paused for a buffer-size change is usually back within it. After that every
/// two seconds, which is quick enough that a replugged interface is running again before an
/// operator has finished looking for the cable, and slow enough that a driver refusing to
/// open is not hammered. A fresh enumeration costs a second or two of loading every ASIO
/// driver on the machine, so it is rarer still.
constexpr double kOutageFirstTrySeconds = 1.0;
constexpr double kOutageRetrySeconds = 2.0;
constexpr double kOutageRescanSeconds = 10.0;

/// Where a row's MIDI dropdown sits for this device — an index into the window's
/// `output-devices`, whose entry 0 is its own "not chosen yet" label.
///
/// Zero for a device this machine has not got, which is a preset from another rig: the row's
/// `address` still names it and still tries to open it, so the name is not lost, and the
/// failure is reported by `applyTargets` rather than hidden behind a dropdown that would
/// otherwise claim the row had no device at all.
int deviceIndexOf(const std::vector<std::string>& ports, const std::string& device) {
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i] == device) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// A target as the boxes of its row hold it.
///
/// The name box is left **empty** when the target is named after its own address, which is
/// what an unnamed one is called (`parseOutputTarget`). Filling it in with the address would
/// be true and useless: it is the box the operator types a name into, and it would come back
/// holding a copy of the box beside it every time they did not.
/// "0, 1, 4" — an Art-Net target's universe list as its box holds it. Empty for a node fed
/// everything, which is the default and what the box's placeholder explains.
std::string universeList(const std::vector<std::uint16_t>& universes) {
    std::string text;
    for (const std::uint16_t universe : universes) {
        if (!text.empty()) {
            text += ", ";
        }
        text += std::to_string(static_cast<unsigned int>(universe));
    }
    return text;
}

OutputRow rowOf(const output::OutputTarget& target, const std::vector<std::string>& midiPorts) {
    OutputRow row{};
    row.id = shared(target.id);
    const std::string address = output::formatOutputAddress(target);
    // Through `shared`: a MIDI target is named after its device unless somebody named it, and
    // the device's name is RtMidi's, in whatever encoding the driver gave it.
    row.name = shared(target.name == address ? std::string{} : target.name);
    row.address = shared(address);
    const bool midi = target.kind == output::OutputTarget::Kind::Midi;
    row.kind_index = static_cast<int>(target.kind);
    row.host = shared(target.host);
    row.port = shared(std::to_string(target.port));
    row.device_index = midi ? deviceIndexOf(midiPorts, target.device) : 0;
    row.universes = shared(universeList(target.universes));
    row.enabled = target.enabled;
    row.delay_ms = static_cast<float>(target.delaySeconds * 1000.0);
    return row;
}

/// The one string that says a row's whole destination, from the fields that are now edited
/// separately — `output::parseOutputTarget`'s own input, which is what `applyTargets` reads
/// and what a settings file holds.
///
/// Empty for a row that is not a destination yet: no host typed, or MIDI with no device
/// picked. Empty is how `applyTargets` already spells "this row is being filled in", so an
/// unfinished row costs no error message and sends nothing.
std::string addressOf(const OutputRow& row, const std::vector<std::string>& midiPorts) {
    if (row.kind_index == static_cast<int>(output::OutputTarget::Kind::Midi)) {
        const auto device = static_cast<std::size_t>(row.device_index);
        if (row.device_index <= 0 || device > midiPorts.size()) {
            return {};
        }
        return "midi " + midiPorts[device - 1];
    }
    const std::string host(row.host);
    if (host.empty()) {
        return {};
    }
    const bool artnet = row.kind_index == static_cast<int>(output::OutputTarget::Kind::ArtNet);
    const std::string port(row.port);
    const std::string where =
        host + ":" +
        (port.empty() ? std::to_string(artnet ? dmx::kArtNetPort : kNewTargetPort) : port);
    if (!artnet) {
        return where;
    }
    // The universe box is free text, so what the operator typed has to be turned into the
    // `u0,1,4` the line format uses — and anything that is not a universe is dropped rather
    // than making the whole row unparseable. A box being typed into holds "0, " for a moment.
    std::string list;
    const std::string typed(row.universes);
    std::size_t at = 0;
    while (at < typed.size()) {
        const std::size_t comma = typed.find(',', at);
        const std::string_view field = trim(
            std::string_view(typed).substr(at, comma == std::string::npos ? comma : comma - at));
        unsigned int universe = 0;
        const char* const begin = field.data();
        const char* const end = begin + field.size();
        if (!field.empty() && std::from_chars(begin, end, universe).ec == std::errc{} &&
            universe <= dmx::kMaxPortAddress) {
            list += list.empty() ? "" : ",";
            list += std::to_string(universe);
        }
        if (comma == std::string::npos) {
            break;
        }
        at = comma + 1;
    }
    return "artnet " + where + (list.empty() ? "" : " u" + list);
}

/// A whole destination back into the fields that edit it, as far as the text allows.
///
/// The parts are the truth now and `addressOf` composes from them — but two ways in still
/// carry the destination whole: a line pasted into a row, and a settings file. Both come
/// through here so the boxes show what arrived rather than staying on whatever they held.
void splitAddress(OutputRow& row, const std::vector<std::string>& midiPorts) {
    const std::string address(row.address);
    output::OutputTarget target;
    if (output::parseOutputTarget(address, target)) {
        const bool midi = target.kind == output::OutputTarget::Kind::Midi;
        row.kind_index = static_cast<int>(target.kind);
        row.host = shared(midi ? std::string{} : target.host);
        row.port = shared(midi ? std::string{} : std::to_string(target.port));
        row.device_index = midi ? deviceIndexOf(midiPorts, target.device) : 0;
        row.universes = shared(universeList(target.universes));
        return;
    }
    // Not a target — a row half-way through being typed, or one whose text was refused. The
    // kind is still readable from the shape of it, and for OSC so is as much of the host and
    // port as has been typed, which is what the boxes should go on showing.
    std::string_view text = trim(address);
    if (text.rfind("midi ", 0) == 0 || text == "midi") {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::Midi);
        return;
    }
    if (text.rfind("artnet ", 0) == 0 || text == "artnet") {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::ArtNet);
        text = trim(text.substr(text.size() > 6 ? 7 : 6));
        const std::size_t marker = text.rfind(" u");
        if (marker != std::string_view::npos) {
            row.universes = shared(std::string(trim(text.substr(marker + 2))));
            text = trim(text.substr(0, marker));
        }
    } else {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::Osc);
    }
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        row.host = shared(std::string(text));
        return;
    }
    row.host = shared(std::string(text.substr(0, colon)));
    row.port = shared(std::string(text.substr(colon + 1)));
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
    // listen at all is machine-local. The same checked prefix the transports get, so the two
    // directions cannot disagree about a bad one.
    config.prefix = usablePrefix(settings);
    return config;
}

} // namespace

WindowController::WindowController(engine::LiveTracker& tracker)
    : WindowController(tracker, settings::Settings{}) {}

namespace {

settings::Settings withIds(settings::Settings settings) {
    settings::assignIds(settings.preset);
    return settings;
}

} // namespace

WindowController::WindowController(engine::LiveTracker& tracker, const settings::Settings& settings)
    : WindowController(tracker, withIds(settings), IdsAssigned{}) {}

WindowController::WindowController(engine::LiveTracker& tracker, const settings::Settings& settings,
                                   IdsAssigned)
    : tracker_(tracker), window_(MainWindow::create()), trace_(kTraceLength),
      traceModel_(
          std::make_shared<slint::VectorModel<TracePoint>>(std::vector<TracePoint>(kTraceLength))),
      targetModel_(std::make_shared<slint::VectorModel<OutputRow>>()),
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
      editor_(runner_, settings.preset.rules),
      // And the lighting patch, the same way. The runner was built with it (see
      // `transportConfig`), so this one really is only the editor's copy.
      patch_(runner_, settings.preset.fixtures) {
    // §4.3's stamp is taken on the audio thread, so the clock has to be installed before a
    // stream is opened. Handing it to the tracker rather than to the engine is what makes
    // that ordering `LiveTracker::start`'s business instead of this class's.
    tickProbe_ = environmentPath("TAKT4_TICK_PROBE");
    meters_ = settings.preset.meters;
    remembered_ = settings.machine;
    midiClockWanted_ = settings.machine.midiClockPort;
    tracker_.setHostTimeSource(&runner_.hostTimeClock());
    midiPorts_ = output::listMidiOutputPorts();
    midiInputPorts_ = output::listMidiInputPorts();

    window_->set_trace(traceModel_);
    window_->set_outputs_list(targetModel_);
    // Set once and never again: the build does not change while it runs. It reaches the
    // title bar and the corner of the status bar, so "which build is this?" is answerable
    // at a glance and stays answerable — the opening status line used to be the only place
    // it was said, and the first status after it took the answer away.
    window_->set_version(slint::SharedString(versionLabel(buildInfo())));

    // §5.6's targets as the last run left them, into the rows that edit them. Seeded once:
    // the drafts are the window's copy from here on, because `publishOutputs` runs thirty
    // times a second while the tracker does and would otherwise replace a row mid-word.
    // From the settings rather than the runner, which has none yet — see `transportConfig`;
    // they reach it below, once there is a status line to report a failure on.
    for (const output::OutputTarget& target : settings.preset.outputs) {
        targetDrafts_.push_back(rowOf(target, midiPorts_));
    }
    publishTargetRows();

    // A pick from the pickers is the operator's choice, which is what makes it the one saved
    // rather than the remembered interface a launch without it fell back from.
    window_->on_device_picked([this](int index) {
        deviceChosen_ = true;
        pickDevice(index);
    });
    window_->on_channel_picked([this](int index) {
        deviceChosen_ = true;
        pickChannel(index);
    });
    window_->on_toggle_run([this] { toggleRun(); });
    window_->on_rescan_clicked([this] { rescanDevices(); });

    window_->on_halve([this] { halve(); });
    window_->on_redouble([this] { redouble(); });
    window_->on_tap([this] { tap(); });
    window_->on_snap_downbeat([this] { snapDownbeat(); });
    window_->on_manual_fired([this] { runner_.post(output::OutputCommand::manual()); });
    window_->on_pin_changed([this](bool pinned) { setPinned(pinned); });
    window_->on_fold_on_changed([this](bool on) { setFoldEnabled(on); });
    window_->on_fold_min_changed([this](float bpm) { setFoldMin(static_cast<double>(bpm)); });
    window_->on_fold_max_changed([this](float bpm) { setFoldMax(static_cast<double>(bpm)); });
    window_->on_latency_changed([this](float ms) { setLatencyMs(static_cast<double>(ms)); });
    window_->on_keep_shift_changed([this](bool keep) { setKeepShift(keep); });

    window_->on_link_toggled([this](bool on) { setLinkEnabled(on); });
    window_->on_output_name_edited([this](int index, const slint::SharedString& name) {
        setTargetName(index, std::string(name), false);
    });
    window_->on_output_name_accepted([this](int index, const slint::SharedString& name) {
        setTargetName(index, std::string(name), true);
    });
    window_->on_output_host_edited([this](int index, const slint::SharedString& host) {
        setTargetHost(index, std::string(host), false);
    });
    window_->on_output_host_accepted([this](int index, const slint::SharedString& host) {
        setTargetHost(index, std::string(host), true);
    });
    window_->on_output_port_edited([this](int index, const slint::SharedString& port) {
        setTargetPort(index, std::string(port), false);
    });
    window_->on_output_port_accepted([this](int index, const slint::SharedString& port) {
        setTargetPort(index, std::string(port), true);
    });
    window_->on_output_kind_changed([this](int index, int kind) { setTargetKind(index, kind); });
    window_->on_output_device_picked(
        [this](int index, int device) { setTargetDevice(index, device); });
    window_->on_output_universes_edited([this](int index, const slint::SharedString& text) {
        setTargetUniverses(index, std::string(text), false);
    });
    window_->on_output_universes_accepted([this](int index, const slint::SharedString& text) {
        setTargetUniverses(index, std::string(text), true);
    });
    window_->on_output_added([this] { addTarget(); });
    window_->on_save_now([this] { saveNow(); });
    window_->on_export_settings([this] {
        // The dialog runs its own message loop, so this must be the UI thread — which a
        // Slint callback is. An empty path is a cancel and `exportTo` does nothing with it.
        exportTo(askSaveFile("Export takt4 settings", "takt4-settings.json"));
    });
    window_->on_import_settings([this] { importFrom(askOpenFile("Import takt4 settings", "")); });
    window_->on_output_removed([this](int index) { removeTarget(index); });
    window_->on_output_enabled_changed([this](int index, bool on) { setTargetEnabled(index, on); });
    window_->on_output_delay_changed([this](int index, float ms) { setTargetDelay(index, ms); });
    window_->on_midi_port_picked([this](int index) { pickMidiPort(index); });

    window_->on_midi_in_picked([this](int index) { pickMidiControlPort(index); });
    window_->on_learn_action_picked([this](int index) { pickLearnAction(index); });
    window_->on_learn_clicked([this] { toggleLearn(); });
    window_->on_forget_clicked([this] { forgetLearned(); });

    window_->on_osc_control_toggled([this](bool on) { setOscControlEnabled(on); });
    window_->on_osc_control_port_edited([this](const slint::SharedString& text) {
        setOscControlPort(readPort(std::string(text)));
    });
    window_->on_osc_control_network_toggled([this](bool on) { setOscControlNetwork(on); });

    window_->on_rules_clicked([this] { openEditor(); });
    window_->on_about_opened([this] { openAbout(); });
    window_->on_fixtures_clicked([this] { patch_.show(); });
    window_->on_panic_clicked([this] { engagePanic(); });
    window_->on_panic_released([this] { releasePanic(); });

    // The editor owns the editing and this owns the file, so a change there comes back
    // here rather than the editor knowing where settings live.
    editor_.setRulesChanged(
        [this](const std::vector<trigger::Rule::Config>& rules) { rules_ = rules; });
    // The same for the patch — and one more thing: a rule aims at a fixture by *name*, so the
    // rule editor's "send to" list has to be rebuilt whenever the patch changes or a rule will
    // go on offering a fixture that has been renamed out from under it.
    patch_.setPatchChanged([this](const std::vector<dmx::Fixture>& fixtures) {
        fixtures_ = fixtures;
        editor_.setPatch(fixtures_);
    });

    publishControlLimits(*window_);
    window_->set_tap_needs(static_cast<int>(taps_.options().needTaps));

    publishPortLists();

    auto kinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    // In `output::OutputTarget::Kind`'s own order, which is what `OutputRow::kind-index` is.
    kinds->push_back(shared("OSC"));
    kinds->push_back(shared("MIDI"));
    kinds->push_back(shared("Art-Net"));
    window_->set_output_kinds(kinds);

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
    // The outputs, for the same reason and with more riding on it. Posted whole, exactly as
    // they were saved: `Transports::setOutputs` opens every target it can and names the ones it
    // could not, so one missing MIDI interface or one hostname that does not resolve yet costs
    // that one output and says so, and the rest of the rig is sending.
    if (!settings.preset.outputs.empty()) {
        runner_.post(output::OutputCommand::outputs(settings.preset.outputs));
        const std::string error = runner_.lastError();
        outputErrorShown_ = error;
        if (!error.empty()) {
            setStatus("outputs: " + error, true);
        }
    }
    if (usablePrefix(settings) != settings.preset.oscPrefix) {
        setStatus("The OSC prefix \"" + settings.preset.oscPrefix +
                      "\" is not an address, so " + usablePrefix(settings) + " is used instead.",
                  true);
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
    // The patch is already on the output thread — `transportConfig` gave it to the runner's
    // constructor, so the universes exist before the first frame. What is left is this
    // class's own copy for saving, and the rule editor's list of what a rule may aim at.
    fixtures_ = settings.preset.fixtures;
    editor_.setPatch(fixtures_);
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

    // **Closing this window closes takt4.** Slint's event loop runs until the *last* window
    // is hidden, and §5.9's editor is a window of its own — so closing the main window with
    // the editor up left the process alive with nothing but the editor on screen, and the
    // settings unwritten, since `ui::run` only saves once the loop has returned. An operator
    // who has just closed the app and been told nothing was kept has met the one failure the
    // SAVE button was added for.
    //
    // **Every other window, not only the editor** — the patch editor is a window of its own
    // too, and closing the main window with PATCH LIGHTS open left takt4 running with nothing
    // on screen but that: the ASIO device, Link and the ports still held, nothing saved, and
    // a relaunch finding the interface busy (the audit's H14).
    window_->window().on_close_requested([this] {
        editor_.hide();
        patch_.hide();
        return slint::CloseRequestResponse::HideWindow;
    });

    // Before the first `show()`, which is what makes it stick — see `kMainWindowWidth`. The
    // markup's `preferred-width` does not size a Slint window; its content does, and with a
    // couple of output rows the content wanted more height than the window had, so the status
    // bar — which is where the version lives — was cut off the bottom.
    // And no taller than the screen has room for — see `fitToScreen` (the audit's M26).
    const LogicalExtent opening = fitToScreen({kMainWindowWidth, kMainWindowHeight});
    window_->window().set_size(slint::LogicalSize({opening.width, opening.height}));

    // **The outputs from now until the window goes**, not from Start to Stop — the audit's H5
    // and the operator's call of 2026-09-23. Last, after every setting above has been applied
    // on this thread: a `post` to a running runner is taken by its thread a millisecond later,
    // and the lines above read `lastError()` straight after posting.
    //
    // Caught, because this is the constructor of the window an operator is about to see: a
    // failure here is said on the status line rather than taking the application with it.
    try {
        runner_.start();
    } catch (const std::exception& e) {
        setStatus(std::string("Cannot start the outputs: ") + e.what(), true);
    }

    // **Every error the window met on the way up, not only the last** (the audit's M25). Each
    // `setStatus` replaces the one before, so a launch that found the MIDI clock port missing
    // and the OSC control port taken said only the second, and the first was never seen.
    constructing_ = false;
    if (startupErrors_.size() > 1) {
        std::string all;
        for (const std::string& error : startupErrors_) {
            all += (all.empty() ? "" : "  ·  ") + error;
        }
        setStatus(all, true);
    }
    startupErrors_.clear();

    timer_.start(slint::TimerMode::Repeated, kRedrawInterval, [this] { tick(); });
}

WindowController::~WindowController() {
    // Before any member goes: `runner_` owns the clock the audio thread reads on every hop,
    // and it is destroyed long before `tracker_`, which is not this class's. `ui::run` stops
    // the tracker itself before letting go of the window; a test that fails half-way through a
    // run does not, and that was a use-after-free on the audio thread.
    tracker_.stop();
    tracker_.setHostTimeSource(nullptr);
    tracker_.engine().setHostTimeSource(nullptr);
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
    // Only a fallback when there was something to fall back *from*. See `deviceFallback_`.
    deviceFallback_ = !restored && !remembered.deviceName.empty();
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

void WindowController::publishPortLists() {
    // The first entry of each list is "nothing picked". **It says so in words**: it used to
    // be an empty string, and a dropdown showing nothing at all does not read as a list
    // nobody has chosen from — it reads as a box the application failed to fill in. Reported
    // from a rig about the control input, and the clock picker had the same hole.
    auto ports = std::make_shared<slint::VectorModel<slint::SharedString>>();
    ports->push_back(shared("no MIDI clock"));
    for (const std::string& port : midiPorts_) {
        ports->push_back(shared(port));
    }
    window_->set_midi_ports(ports);

    // The same devices again, under the label a *target* row wants: leaving the clock unset
    // is a setting, leaving a target's device unset is an unfinished row.
    auto devices = std::make_shared<slint::VectorModel<slint::SharedString>>();
    devices->push_back(
        shared(midiPorts_.empty() ? "no MIDI outputs on this machine" : "select a MIDI device"));
    for (const std::string& port : midiPorts_) {
        devices->push_back(shared(port));
    }
    window_->set_output_devices(devices);

    auto inputs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    inputs->push_back(
        shared(midiInputPorts_.empty() ? "no MIDI inputs on this machine" : "select input"));
    for (const std::string& port : midiInputPorts_) {
        inputs->push_back(shared(port));
    }
    window_->set_midi_in_ports(inputs);
}

void WindowController::rescanDevices() {
    if (wantRunning_ || tracker_.running()) {
        setStatus("Stop first: devices can only be looked for again while nothing is open.", true);
        return;
    }
    // What to land on afterwards: what the operator chose, or — when the last read fell back
    // from the remembered interface and nobody picked another — the remembered one, which is
    // the whole point of looking again after powering it on.
    settings::MachineSettings keep = remembered_;
    if ((!deviceFallback_ || deviceChosen_) && device_ >= 0 &&
        static_cast<std::size_t>(device_) < devices_.size()) {
        keep.deviceName = devices_[static_cast<std::size_t>(device_)].name;
        keep.hostApiName = devices_[static_cast<std::size_t>(device_)].hostApiName;
        keep.channel = channel_;
    }
    setStatus("Looking for devices...", false);
    try {
        (void)tracker_.rescan();
    } catch (const std::exception& e) {
        setStatus(std::string("Could not look for devices again: ") + e.what(), true);
        return;
    }
    refreshDevices(keep);

    // RtMidi enumerates afresh on every call, so these are simply read again. The target rows
    // are re-derived from their names by `applyTargets` below, which is what keeps a row on
    // its device when the list reorders.
    midiPorts_ = output::listMidiOutputPorts();
    midiInputPorts_ = output::listMidiInputPorts();
    publishPortLists();

    // Whatever was remembered and missing may be here now.
    if (!midiClockWanted_.empty() && !runner_.snapshot().midiClockOpen) {
        setMidiPort(midiClockWanted_);
    }
    if (!control_.running() && !control_.config().port.empty()) {
        setMidiControlPort(control_.config().port);
    }
    applyTargets();
    publishControl();
    if (!statusIsError_) {
        setStatus("Found " + std::to_string(devices_.size()) + " inputs, " +
                      std::to_string(midiPorts_.size()) + " MIDI outputs and " +
                      std::to_string(midiInputPorts_.size()) + " MIDI inputs.",
                  false);
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
    // By what was asked for, not by whether a stream is open: during an outage the tracker is
    // stopped between attempts to reopen it, and STOP has to mean stop — not "start".
    if (wantRunning_ || tracker_.running()) {
        wantRunning_ = false;
        outage_.reset();
        input_.reset();
        window_->set_input_lost(false);
        window_->set_input_trouble(shared(""));
        // The tracker first: it tracks the hops still in flight on the way down, and those
        // can call a last beat that the runner should still send. The runner itself goes on —
        // it runs for the application's whole life (the audit's H5) — and is told the tracker
        // stopped, which stops the MIDI clock and blacks the lights out (Q3).
        tracker_.stop();
        runner_.setTracking(false);
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
    // After the tracker, so the MIDI clock is not given a Start for a run that failed to open.
    runner_.setTracking(true);
    wantRunning_ = true;
    input_ = tracker_.current();
    watchdog_.reset(tracker_.stream()->sampleRate(), nowSeconds());
    overflowsShown_ = 0;
    window_->set_input_trouble(shared(""));
    window_->set_running(true);
    publishOpenStream();
}

bool WindowController::reopenInput(std::string& error) {
    if (!input_) {
        error = "nothing was open";
        return false;
    }
    tracker_.stop();
    // Found again by name: a rescan renumbers every device, and the index the input had when
    // it was opened may now belong to something else.
    const audio::InputDevice* device = &input_->device;
    for (const audio::InputDevice& candidate : devices_) {
        if (candidate.name == input_->device.name &&
            candidate.hostApiName == input_->device.hostApiName) {
            device = &candidate;
            break;
        }
    }
    try {
        tracker_.start(*device, input_->selection);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    input_ = tracker_.current();
    watchdog_.reset(tracker_.stream()->sampleRate(), nowSeconds());
    overflowsShown_ = 0;
    window_->set_input_trouble(shared(""));
    return true;
}

void WindowController::beginOutage(const std::string& why, double since, double now) {
    Outage outage;
    outage.since = since;
    outage.nextTry = now + kOutageFirstTrySeconds;
    outage.why = why;
    outage_ = outage;
    window_->set_input_lost(true);
    // The meter goes to nothing rather than holding its last reading: it is the first thing an
    // operator looks at to see whether sound is arriving, and it used to say it was.
    peak_ = 0.0f;
    window_->set_input_level(0.0f);
    window_->set_input_peak(0.0f);
    window_->set_input_reading(shared("no audio"));
    setStatus(why + " — the interface stopped sending. Reopening it...", true);
}

void WindowController::restartInput(const std::string& why, double now) {
    std::string error;
    if (reopenInput(error)) {
        const audio::InputStream* stream = tracker_.stream();
        // Not red: it is fixed, and an error colour on a status that asks for nothing stays
        // there until something else is said (the audit's M25).
        setStatus(why + "; reopened at " + fixed(stream->sampleRate(), 0) + " Hz.", false);
        return;
    }
    beginOutage(why + ", and it would not open again (" + error + ")", now, now);
}

void WindowController::superviseInput(const audio::InputWatchdog::Reading& reading,
                                      const audio::AsioDriverEvents& events, double now) {
    if (!wantRunning_) {
        return;
    }
    const std::string name = input_ ? input_->device.name : std::string("the input");

    if (outage_) {
        if (now < outage_->nextTry) {
            return;
        }
        ++outage_->tries;
        // Every third try, and not more often than `kOutageRescanSeconds`, PortAudio looks at
        // the machine again: an interface that was unplugged, or power-cycled, can come back
        // under another index or only be seen again by a fresh enumeration. The tracker is
        // stopped for it — which it already is between attempts — and the device found again
        // by name in `reopenInput`.
        if (outage_->tries % 3 == 0 &&
            (rescannedAt_ < 0.0 || now - rescannedAt_ >= kOutageRescanSeconds)) {
            rescannedAt_ = now;
            tracker_.stop();
            try {
                if (tracker_.rescan()) {
                    devices_ = tracker_.devices();
                }
            } catch (const std::exception&) {
                // PortAudio would not come back up; the next try will ask again.
            }
        }
        std::string error;
        if (reopenInput(error)) {
            const double lasted = now - outage_->since;
            outage_.reset();
            window_->set_input_lost(false);
            setStatus("Audio is back from " + name + " after " + fixed(lasted, 0) +
                          " s without it; reopened at " +
                          fixed(tracker_.stream()->sampleRate(), 0) + " Hz.",
                      false); // fixed, so not red — see `restartInput`
        } else {
            outage_->nextTry = now + kOutageRetrySeconds;
            setStatus(outage_->why + " — still no audio after " +
                          fixed(now - outage_->since, 0) + " s (" + error + "). Trying again...",
                      true);
        }
        return;
    }

    // What the ASIO driver said. Each of these means the stream as opened no longer describes
    // the hardware, and PortAudio's host used to acknowledge them and carry on regardless.
    if (events.needsReopen()) {
        const char* what = events.resetRequest       ? "asked to be reset"
                           : events.sampleRateChange ? "changed its sample rate"
                                                     : "changed its buffer size";
        restartInput("The driver for " + name + " " + what, now);
        return;
    }

    if (reading.inputOverflows != overflowsShown_) {
        overflowsShown_ = reading.inputOverflows;
        window_->set_input_trouble(shared(
            overflowsShown_ == 0
                ? std::string{}
                : std::to_string(overflowsShown_) +
                      (overflowsShown_ == 1 ? " input overflow" : " input overflows") +
                      " — the interface dropped audio"));
    }

    switch (reading.verdict) {
    case audio::InputWatchdog::Verdict::Silent:
        beginOutage("No audio from " + name, now - reading.silentForSeconds, now);
        break;
    case audio::InputWatchdog::Verdict::RateChanged: {
        const double opened = tracker_.stream() != nullptr ? tracker_.stream()->sampleRate() : 0.0;
        // What an operator hears as "every tempo is slightly wrong": the resampler converting
        // at the old ratio from a clock another program has moved.
        restartInput("The clock of " + name + " moved from " + fixed(opened, 0) + " Hz to about " +
                         fixed(reading.measuredRate, 0) + " Hz",
                     now);
        break;
    }
    case audio::InputWatchdog::Verdict::Starting:
    case audio::InputWatchdog::Verdict::Healthy:
        break;
    }
}

void WindowController::setLinkEnabled(bool on) {
    // Waited for, so the row below is drawn from what the transports now are rather than from
    // the snapshot before this change — the runner is always running, so a plain `post` is
    // taken a round later.
    (void)runner_.postAndWait(output::OutputCommand::linkEnabled(on));
    publishOutputs();
}

void WindowController::editTarget(int index, const std::string& name, const std::string& address) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    row.name = shared(name);
    row.address = shared(address);
    // The boxes that edit the two halves of that address, so a whole destination arriving
    // this way — a pasted line, a settings file — is shown in the fields it is made of.
    splitAddress(row, midiPorts_);
    // Deliberately no `applyTargets` and no `publishTargetRows`: this is one keystroke.
    // Applying would rebuild a socket per character, and publishing would re-evaluate the
    // `text:` binding of the box the operator is inside.
}

void WindowController::acceptTarget(int index, const std::string& name,
                                    const std::string& address) {
    editTarget(index, name, address);
    applyTargets();
}

void WindowController::setTargetName(int index, const std::string& name, bool apply) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    targetDrafts_[static_cast<std::size_t>(index)].name = shared(name);
    if (apply) {
        applyTargets();
    }
}

void WindowController::setTargetHost(int index, const std::string& host, bool apply) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    row.host = shared(host);
    row.address = shared(addressOf(row, midiPorts_));
    if (apply) {
        applyTargets();
    }
}

void WindowController::setTargetPort(int index, const std::string& port, bool apply) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    row.port = shared(port);
    row.address = shared(addressOf(row, midiPorts_));
    if (apply) {
        applyTargets();
    }
}

void WindowController::setTargetKind(int index, int kind) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    if (row.kind_index == kind) {
        return;
    }
    const bool wasArtNet = row.kind_index == static_cast<int>(output::OutputTarget::Kind::ArtNet);
    row.kind_index = kind;
    // Art-Net has one port and everybody uses it. A row switched to it while holding 7000 —
    // an OSC port an operator typed, or the default a new row is born with — would be a node
    // that never answers, and the reason would be a number they had no reason to look at. So
    // the conventional port is put in when the kind is picked, and put back when it is not.
    if (kind == static_cast<int>(output::OutputTarget::Kind::ArtNet)) {
        row.port = shared(std::to_string(dmx::kArtNetPort));
    } else if (wasArtNet) {
        row.port = shared(std::to_string(kNewTargetPort));
    }
    // A row switched to MIDI has no device picked yet, and one switched back to OSC keeps
    // whatever host and port it had — so `addressOf` gives an empty destination for the
    // first and the old one back for the second. Empty is how `applyTargets` spells "still
    // being filled in", so switching kind never raises an error about an unfinished row.
    row.address = shared(addressOf(row, midiPorts_));
    // Applied at once, unlike a keystroke: picking from a list is a finished decision, and
    // the row has to redraw as the other kind either way.
    applyTargets();
}

void WindowController::setTargetDevice(int index, int device) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    row.device_index = device;
    row.kind_index = static_cast<int>(output::OutputTarget::Kind::Midi);
    row.address = shared(addressOf(row, midiPorts_));
    applyTargets();
}

void WindowController::setTargetUniverses(int index, const std::string& universes, bool apply) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    // Kept exactly as typed. `addressOf` is what turns it into the line format, and it drops
    // anything that is not a universe — so a box holding "0, " mid-edit is a node on universe
    // 0 rather than a row that has stopped parsing.
    row.universes = shared(universes);
    row.address = shared(addressOf(row, midiPorts_));
    if (apply) {
        applyTargets();
    }
}

void WindowController::addTarget() {
    OutputRow row{};
    row.enabled = true;
    // A destination on this machine at the conventional port, rather than the blank row this
    // used to add. A blank row applies nothing, so the target did not exist until the whole
    // address had been typed — and a rule cannot be routed to a target that is not there
    // yet, which is the wrong way round for somebody building a rig. This one is a working
    // target from the moment it appears; both boxes are then edited like any other.
    row.kind_index = 0;
    row.host = shared("127.0.0.1");
    row.port = shared(std::to_string(kNewTargetPort));
    row.address = shared(addressOf(row, midiPorts_));
    // Its id from the moment it exists, so a rule can be routed to it before it is renamed and
    // stays routed after. Checked against the outputs running now as well as the rows, since a
    // row being filled in has not reached the runner yet.
    std::vector<output::OutputTarget> known = runner_.snapshot().outputs;
    for (const OutputRow& other : targetDrafts_) {
        output::OutputTarget holder;
        holder.id = std::string(other.id);
        known.push_back(std::move(holder));
    }
    row.id = shared(output::newOutputId(known));
    targetDrafts_.push_back(row);
    // Anything already typed into the rows above survives because `editTarget` kept it here.
    applyTargets();
}

void WindowController::removeTarget(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    targetDrafts_.erase(targetDrafts_.begin() + index);
    applyTargets();
}

void WindowController::setTargetEnabled(int index, bool on) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    targetDrafts_[static_cast<std::size_t>(index)].enabled = on;
    applyTargets();
}

void WindowController::setTargetDelay(int index, float ms) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    const float clamped =
        std::clamp(ms, static_cast<float>(output::kMinOutputDelaySeconds * 1000.0),
                   static_cast<float>(output::kMaxOutputDelaySeconds * 1000.0));
    OutputRow& draft = targetDrafts_[static_cast<std::size_t>(index)];
    if (draft.delay_ms == clamped) {
        return; // the slider resending where it already is; see the fold sliders
    }
    draft.delay_ms = clamped;
    // Applied as it moves, so the offset can be found by ear — as the delay alone, by the
    // output's id. A whole target list per pixel of the drag re-applied every output, flushed
    // what was held for the others and, before that, rebuilt every sender (the audit's H12).
    // A row that is not an output yet — still being typed — has nothing to move, and goes the
    // long way when it becomes one.
    const std::string id(draft.id);
    if (!id.empty() && output::findTarget(runner_.snapshot().outputs, id) != nullptr) {
        runner_.post(output::OutputCommand::outputDelay(id, static_cast<double>(clamped) / 1000.0));
        publishTargetRows();
        publishOutputs();
        return;
    }
    applyTargets();
}

void WindowController::setOscTargets(const std::string& text) {
    // The whole rig as one piece of text, which is what a settings file's line looks like
    // and what somebody pastes. It arrives as a single draft and `applyTargets` splits it
    // into rows, which is the same path a pasted row takes.
    OutputRow row{};
    row.address = shared(text);
    row.enabled = true;
    targetDrafts_.clear();
    targetDrafts_.push_back(row);
    applyTargets();
}

void WindowController::applyTargets() {
    // §5.6's named targets. Each row is `host:port` or `midi Device` with the name in the
    // box beside it, and `output::parseOutputTarget` also takes the whole `name = address`
    // form — which is what a row holds when a line has been pasted into it.
    //
    // A row that will not parse is named on the status line and kept exactly as it was
    // typed; the rest are still applied. An operator halfway through an address must not
    // lose the outputs that already worked.
    std::vector<OutputRow> rows;
    std::vector<output::OutputTarget> targets;
    std::string bad;
    for (const OutputRow& draft : targetDrafts_) {
        const std::string name(draft.name);
        const std::string address(draft.address);
        const std::vector<std::string_view> parts = splitTargets(address);
        if (parts.empty()) {
            // A row just added, or one whose address has been cleared. Kept — it is being
            // filled in — and nothing is sent for it.
            rows.push_back(draft);
            continue;
        }

        std::vector<output::OutputTarget> parsed;
        for (const std::string_view part : parts) {
            output::OutputTarget target;
            if (!output::parseOutputTarget(part, target)) {
                parsed.clear();
                break;
            }
            parsed.push_back(std::move(target));
        }
        if (parsed.empty()) {
            if (bad.empty()) {
                bad = address;
            }
            rows.push_back(draft);
            continue;
        }

        for (std::size_t i = 0; i < parsed.size(); ++i) {
            output::OutputTarget& target = parsed[i];
            // The name box wins over a name inside the address, but only for the row it
            // belongs to: the rest of a pasted line brought its own names.
            if (i == 0 && !name.empty()) {
                target.name = name;
            }
            // And the row's id, which is what keeps it the same output whatever is typed into
            // it. A line pasted into a row brings the rest of its targets with their own ids,
            // or none, and those are given one below.
            if (i == 0 && !std::string(draft.id).empty()) {
                target.id = std::string(draft.id);
            }
            // Both have to agree: the switch is the row's, and "off " in front of a pasted
            // address is that line saying the same thing.
            target.enabled = target.enabled && draft.enabled;
            // The slider owns the row's offset, because `formatOutputAddress` never writes one
            // into the address box — so an offset coming back from the parse can only be one a
            // person just typed or pasted, and that is them saying it outright.
            //
            // Exactly zero, not "not positive": an offset can be negative now, and a pasted
            // `-300ms` is as deliberate as a pasted `+300ms`.
            if (i == 0 && target.delaySeconds == 0.0) {
                target.delaySeconds = static_cast<double>(draft.delay_ms) / 1000.0;
            }
            // An id for one that came without, and a fresh one for a line pasted twice — before
            // the row is built from it, so the row holds the id the runner is given.
            if (target.id.empty() || output::findTarget(targets, target.id) != nullptr) {
                target.id = output::newOutputId(targets);
            }
            rows.push_back(rowOf(target, midiPorts_));
            targets.push_back(std::move(target));
        }
    }

    // Two targets with one name route fine — rules hold ids — but are two rows nobody can
    // tell apart in the rule editor's list. Said, so the operator can rename one.
    std::string duplicate;
    for (std::size_t i = 0; i < targets.size() && duplicate.empty(); ++i) {
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            if (targets[i].name == targets[j].name) {
                duplicate = targets[i].name;
                break;
            }
        }
    }

    targetDrafts_ = std::move(rows);
    // Waited for, so an output that will not open is said now — the runner runs for the
    // application's whole life, so a plain `post` is always answered a round later. When even
    // the wait runs out, `tick` is what notices the answer; see `outputErrorShown_`.
    const std::optional<std::string> answer =
        runner_.postAndWait(output::OutputCommand::outputs(targets));
    const std::string error = answer.value_or(std::string{});
    if (answer) {
        outputErrorShown_ = error;
    }
    if (!bad.empty()) {
        setStatus("outputs: \"" + bad + "\" is not a target, so it was left out.", true);
    } else if (!duplicate.empty()) {
        setStatus("outputs: two are called \"" + duplicate +
                      "\". Rename one so the rule editor can tell them apart.",
                  true);
    } else if (!error.empty()) {
        // A MIDI device that is not on this machine. The rest of the rig is still sending;
        // what failed is one output, and §5.6's whole point is that they are separate.
        setStatus("outputs: " + error, true);
    } else if (statusIsError_) {
        setStatus("Pick an input and press Start.", false);
    }
    publishTargetRows();
    publishOutputs();
}

void WindowController::publishTargetRows() {
    // In place. `applyTargets` calls this, and `setTargetDelay` calls `applyTargets` on every
    // step of a drag — so replacing the model here destroyed and rebuilt the very slider the
    // pointer was holding, and the drag ended on the first pixel of movement. See `writeRows`.
    //
    // But updating in place also keeps a *dead* `text:` binding, and these rows are three text
    // boxes each. Delete the first of three outputs and every row below moves up one: the
    // rows are rewritten, and any box that had been typed into goes on showing the name that
    // belonged to the target above it. The same failure the rule editor's chips had, on the
    // same mechanism — Slint drops a binding the moment the property is assigned.
    //
    // So a row whose *text* moved is rebuilt, and a row where only the slider or the tick
    // moved is not. That keeps the drag alive, which is what this comment started as.
    if (rowsNeedRebuild(
            *targetModel_, targetDrafts_, [](const OutputRow& was, const OutputRow& now) {
                return was.name != now.name || was.host != now.host || was.port != now.port ||
                       was.kind_index != now.kind_index || was.device_index != now.device_index;
            })) {
        targetRowsDirty_ = true;
    }
    writeRows(*targetModel_, targetDrafts_);

    // Which headings the column needs, and how wide the destination slot is. See
    // `outputs-any-artnet`: an Art-Net row is one field wider than the other two.
    const bool artnet = std::any_of(
        targetDrafts_.begin(), targetDrafts_.end(), [](const OutputRow& row) {
            return row.kind_index == static_cast<int>(output::OutputTarget::Kind::ArtNet);
        });
    window_->set_outputs_any_artnet(artnet);
}

void WindowController::setMidiPort(const std::string& name) {
    // Wanted whether or not it opens: see `midiClockWanted_`.
    midiClockWanted_ = name;
    const std::optional<std::string> answer = runner_.postAndWait(output::OutputCommand::midiClockPort(
        name.empty() ? std::optional<std::string>{} : std::optional<std::string>{name}));
    if (answer) {
        outputErrorShown_ = *answer;
        if (!answer->empty()) {
            setStatus("MIDI clock: " + *answer, true);
        }
    }
    publishOutputs();
}

void WindowController::pickMidiPort(int index) {
    // Index 0 is the list's own "no MIDI clock" entry, so the ports start at 1.
    const auto port = static_cast<std::size_t>(index);
    setMidiPort(index <= 0 || port > midiPorts_.size() ? std::string{} : midiPorts_[port - 1]);
}

void WindowController::pickMidiControlPort(int index) {
    const auto port = static_cast<std::size_t>(index);
    setMidiControlPort(index <= 0 || port > midiInputPorts_.size() ? std::string{}
                                                                   : midiInputPorts_[port - 1]);
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

namespace {

std::string_view textOf(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// The machine's own viewer for a text file.
bool openInViewer(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32; // ShellExecute's own convention: anything above 32 is success
#else
    (void)path;
    return false; // said on screen as "written to", which is still somewhere to look
#endif
}

} // namespace

void WindowController::openAbout() {
    if (!about_) {
        about_ = AboutWindow::create();
        AboutWindow& about = **about_;
        about.set_version(slint::SharedString(versionLabel(buildInfo())));
        about.on_licence_opened(
            [this] { (void)openEmbeddedText("takt4-LICENSE.txt", textOf(assets::licence())); });
        about.on_notices_opened([this] {
            (void)openEmbeddedText("takt4-THIRD-PARTY-NOTICES.txt", textOf(assets::notices()));
        });
        about.on_closed([this] { (*about_)->hide(); });
        // The size it was drawn for, before the first show — Slint opens a window at its
        // content's minimum otherwise.
        about.window().set_size(slint::LogicalSize({580.0f, 520.0f}));
    }
    (*about_)->show();
}

std::filesystem::path WindowController::openEmbeddedText(const std::string& name,
                                                         std::string_view text) {
    std::error_code code;
    const std::filesystem::path folder = std::filesystem::temp_directory_path(code) / "takt4";
    std::filesystem::create_directories(folder, code);
    const std::filesystem::path path = folder / name;
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            if (about_) {
                (*about_)->set_opened(slint::SharedString("Could not write " + io::pathText(path)));
            }
            return {};
        }
    }
    const bool opened = openFile_ ? openFile_(path) : openInViewer(path);
    if (about_) {
        (*about_)->set_opened(slint::SharedString(
            opened ? "Opened " + io::pathText(path)
                   : "Written to " + io::pathText(path) + " — open it in any text viewer"));
    }
    return path;
}

void WindowController::engagePanic() {
    // Waited for — a round, a millisecond — so the button is drawn lit, and RELEASE is drawn
    // beside it, by the press itself rather than by the next redraw: the runner is always
    // running, so a plain `post` has not been applied when `publishTriggers` reads it.
    (void)runner_.postAndWait(output::OutputCommand::panic(true));
    publishTriggers();
}

void WindowController::releasePanic() {
    (void)runner_.postAndWait(output::OutputCommand::panic(false));
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
    // What was asked for, before anything can fail: see `oscControlWanted_`.
    oscControlWanted_ = on;
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
    const std::uint16_t previousPort = oscControl_.config().port;
    config.port = static_cast<std::uint16_t>(port);

    // A socket is bound at `start()`, so changing the port means going round again — but
    // only if it was listening. Editing the number while it is off is just editing a
    // number, and must not open a socket nobody asked for.
    const bool wasRunning = oscControl_.running();
    oscControl_.stop();
    config.enabled = false;
    oscControl_.setConfig(config);
    // Wanted but not listening — its port was taken at launch — is a new number to try too:
    // that is usually why the operator is changing it.
    if (!wasRunning && !oscControlWanted_) {
        publishControl();
        return;
    }
    setOscControlEnabled(true);
    if (oscControl_.running() || !wasRunning) {
        return;
    }
    // **The new port would not bind, so the old one comes back** (the audit's M24). This
    // closed the working socket first and then tried the new number, so a typo — or a port
    // another program has — left a Stream Deck with no one listening, mid-show. What
    // `setOscControlEnabled` said about the new port is kept; this adds where it still is.
    const std::string refusal(window_->get_status());
    config.port = previousPort;
    oscControl_.setConfig(config);
    setOscControlEnabled(true);
    if (oscControl_.running()) {
        setStatus(refusal + " Still listening on " + std::to_string(oscControl_.port()) + ".",
                  true);
    }
    publishControl();
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

void WindowController::publishControl(bool force) {
    // Two independent surfaces, published independently. **Not one function with two
    // halves**: the MIDI half returns early when no port is open, and an OSC half written
    // below that return was silently never published — caught by
    // `tests/ui/window_test.cpp`'s "opens only when asked", which found the port field
    // empty on a window whose socket was bound.
    publishMidiControl();
    publishOscControl(force);
}

void WindowController::publishMidiControl() {
    window_->set_control_on(control_.running());
    window_->set_learning(control_.learning().has_value());
    window_->set_learn_action_index(learnAction_);
    // The picker follows the port rather than being the only record of it: it is written
    // from a settings file at startup and by `setMidiControlPort` from anywhere else, and
    // a ComboBox cannot be moved from outside by its value at all (Slint 11970).
    window_->set_midi_in_port_index(deviceIndexOf(midiInputPorts_, control_.config().port));

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
    window_->set_fixtures_total(static_cast<int>(fixtures_.size()));
    window_->set_panicked(runner_.panicked());
    // The editor is the only thing that drains what fired — `OutputRunner::takeFired` is a
    // drain and two readers would each get half — so this row is fed from what the editor
    // saw rather than from the runner. It is published *after* `editor_.tick()` in `tick()`
    // for that reason: reading it before would always be one round behind.
    window_->set_rules_last_fired(shared(editor_.lastFiredAnywhere()));
}

void WindowController::publishOscControl(bool force) {
    const control::OscControl::Config& config = oscControl_.config();
    const bool listening = oscControl_.running();
    window_->set_osc_control_on(listening);
    window_->set_osc_control_network(!config.localOnly);
    // The port bound while it is listening, and the one asked for while it is not. With 0
    // meaning "any free one" those differ, and only the bound one is a number an operator
    // can point a Stream Deck at.
    //
    // **Only when the number underneath has moved.** The markup binds this two-way —
    // `text <=> root.osc-control-port` — so the property *is* the box, and this runs on the
    // redraw timer: writing it unconditionally put the current port back into the field
    // thirty times a second, which erased each digit before the next one could be typed. The
    // field commits on Enter, so there was no way to reach a different port at all. Forced
    // from every path that is an operator doing something, including the one that rejects
    // what they typed, so a refused port is still put back.
    const std::string port = std::to_string(listening ? oscControl_.port() : config.port);
    if (force || port != oscControlPortShown_) {
        oscControlPortShown_ = port;
        window_->set_osc_control_port(shared(port));
    }

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

void WindowController::setKeepShift(bool keep) {
    Options options = settings();
    options.keepOctaveShift = keep;
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
    // **And to the transports**, which is where the latency offset does anything at all: the
    // tracker applies it only to `BeatEvent::time`, which nothing in the application reads.
    // Until the audit (C6) this was the one place it was sent, so dragging "latency" to pull
    // the rig earlier moved nothing — and the value was saved, and took effect at the *next*
    // launch, offsetting a later show by an amount nobody chose that night. One atomic,
    // safe from this thread; every way a setting changes comes through here, the slider and
    // an import alike.
    runner_.setLatencySeconds(options.latencyOffsetSeconds);
    posted_ = options;
    settling_ = 0;
    publishTempoOptions(*window_, options);
}

void WindowController::publishTaps() {
    window_->set_tap_count(static_cast<int>(taps_.taps()));
}

settings::Settings WindowController::currentSettings() const {
    settings::Settings out;
    if (deviceFallback_ && !deviceChosen_) {
        // The remembered interface was not here and nobody picked another — so it is still the
        // one wanted. Saving the fallback as if chosen made one launch before the MOTU was
        // powered on move the next show to the wrong device at channel 1 (the audit's H11).
        out.machine.deviceName = remembered_.deviceName;
        out.machine.hostApiName = remembered_.hostApiName;
        out.machine.channel = remembered_.channel;
    } else if (device_ >= 0 && static_cast<std::size_t>(device_) < devices_.size()) {
        const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
        out.machine.deviceName = device.name;
        out.machine.hostApiName = device.hostApiName;
        out.machine.channel = channel_;
    }
    // **The snapshot, not the live transports.** SAVE and EXPORT are pressed while a set is
    // running, and `OutputRunner::transports()` hands back references the output thread
    // replaces whole — a vector of targets, an optional port. Copying one while that thread
    // reassigns it is a freed buffer, not a stale reading. `publishOutputs` already reads
    // this way; this was the one place left that did not.
    const output::OutputRunner::Snapshot live = runner_.snapshot();
    // The port asked for, open or not: one that was unplugged at this launch is still the one
    // wanted at the next, and saving what happened to open lost it (the audit's H11).
    out.machine.midiClockPort = midiClockWanted_;

    // §5.7's control surface, which is machine-local for the same reason and more so: what
    // was learned describes the box of buttons on this desk.
    out.machine.midiControlPort = control_.config().port;
    for (const control::MidiBinding& binding : control_.bindings()) {
        out.machine.midiBindings.push_back(control::formatMidiBinding(binding));
    }

    // §5.7's other surface: **what was asked for, not whether it bound** (the audit's M24).
    // This used to save `running()`, on the reasoning that a port taken at startup should not
    // be claimed in the file — but a port that was only briefly busy (another copy of takt4
    // still closing, say) then switched OSC control off for every launch after, silently,
    // since nothing tried again: the Stream Deck simply stopped working at the next show. A
    // port that stays taken says so at every launch instead, in the status line.
    // The port asked for, not `port()`: with 0 meaning "any free one", saving what the
    // platform happened to hand out would silently pin next launch to it.
    out.machine.oscControlEnabled = oscControlWanted_;
    out.machine.oscControlPort = oscControl_.config().port;
    out.machine.oscControlLocalOnly = oscControl_.config().localOnly;

    // `settings()`: the engine's own, or a change posted moments ago that it has not taken
    // yet. **Not the engine's alone**, which is what this read until the audit (M27): a
    // stopped engine applies a posted change only at the next Start, so a slider moved and
    // then saved — or autosaved — while stopped wrote the value from *before* the move. A tap
    // still wins, because a tap clears what was in flight (§7 deviation 8).
    out.preset.tempo = settings();
    // What the next launch should use: an imported preset's, where it named one this run
    // cannot switch to — see `importFrom`.
    out.preset.decoder = pendingDecoder_.value_or(tracker_.engine().decoderKind());
    out.preset.meters = meters_;
    out.preset.link = live.link;
    out.preset.outputs = live.outputs;
    out.preset.oscPrefix = pendingPrefix_.value_or(live.oscPrefix);
    // This window's copy, not the runner's: the runner's belong to the output thread and
    // reading them while it runs is what `rules()` explains is unsafe. The patch is the same
    // — the editor's copy, kept in step by its changed callback.
    out.preset.rules = rules_;
    out.preset.fixtures = fixtures_;
    return out;
}

bool WindowController::saveNow() {
    const std::filesystem::path path = settings::settingsFile();
    if (path.empty()) {
        setStatus("There is nowhere to save settings on this machine.", true);
        return false;
    }
    // Every path shown through `io::pathText`, never `path.string()` — which converts through
    // the ANSI code page and either throws or produces bytes Slint aborts on for a folder
    // called "Shows – 2026". The audit's H13.
    const std::string text = settings::toJson(currentSettings());
    if (!settings::saveText(text, path)) {
        setStatus("Could not write " + io::pathText(path), true);
        return false;
    }
    // The autosave's idea of what is on disk, when this was its file: otherwise it would
    // write the same bytes again a few seconds later.
    if (path == autosaveFile_) {
        savedText_ = text;
        pendingText_.clear();
    }
    setStatus("Saved to " + io::pathText(path), false);
    return true;
}

bool WindowController::exportTo(const std::filesystem::path& path) {
    if (path.empty()) {
        return false; // cancelled
    }
    if (!settings::save(currentSettings(), path)) {
        setStatus("Could not write " + io::pathText(path), true);
        return false;
    }
    setStatus("Exported to " + io::pathText(path), false);
    return true;
}

void WindowController::enableAutosave(std::filesystem::path file, double quietSeconds) {
    autosaveFile_ = std::move(file);
    autosaveQuietSeconds_ = std::max(0.0, quietSeconds);
    // What the file holds now, as far as this window can know: the settings it was built
    // from, as it would write them. A difference from this is a change worth saving.
    savedText_ = autosaveFile_.empty() ? std::string{} : settings::toJson(currentSettings());
    pendingText_.clear();
    autosaveFailedAt_ = -1.0;
}

void WindowController::showNotice(const std::string& text) {
    if (!text.empty()) {
        setStatus(text, true);
    }
}

void WindowController::autosave(double now) {
    // A full disk or a folder gone read-only is retried every ten seconds, not twice a second:
    // each attempt is a file created and thrown away.
    constexpr double kRetrySeconds = 10.0;
    std::string text = settings::toJson(currentSettings());
    if (text == savedText_) {
        pendingText_.clear();
        return;
    }
    if (text != pendingText_) {
        // Still moving — a drag, a word being typed. Wait for it to settle.
        pendingText_ = std::move(text);
        pendingSince_ = now;
        return;
    }
    if (now - pendingSince_ < autosaveQuietSeconds_) {
        return;
    }
    if (autosaveFailedAt_ >= 0.0 && now - autosaveFailedAt_ < kRetrySeconds) {
        return;
    }
    if (!settings::saveText(pendingText_, autosaveFile_)) {
        // Said once, when it starts failing — and in words that tell an operator the rig is
        // not being kept, which is the thing they have to act on.
        if (autosaveFailedAt_ < 0.0) {
            setStatus("Could not save settings to " + io::pathText(autosaveFile_) +
                          " — changes are not being kept. Trying again every few seconds.",
                      true);
        }
        autosaveFailedAt_ = now;
        return;
    }
    if (autosaveFailedAt_ >= 0.0) {
        setStatus("Settings saved to " + io::pathText(autosaveFile_) + " again.", false);
    }
    autosaveFailedAt_ = -1.0;
    savedText_ = std::move(pendingText_);
    pendingText_.clear();
}

bool WindowController::importFrom(const std::filesystem::path& path) {
    if (path.empty()) {
        return false; // cancelled
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        setStatus("No such file: " + io::pathText(path), true);
        return false;
    }
    // `settings::load` is documented never to fail: anything it cannot read gives defaults.
    // That is right for startup and wrong here — importing a JPEG would silently wipe the
    // rules — so the file is checked for being *settings* before any of it is applied.
    const settings::Settings loaded = settings::load(path);
    // **The whole preset half against a fresh install's**, not four fields of it: this used to
    // look at the rules, the outputs, the prefix and the meters, so a file holding nothing but
    // a lighting patch — or a tempo window, or a decoder — was "no preset" (the audit's M18).
    {
        settings::Settings imported;
        imported.preset = loaded.preset;
        if (settings::toJson(imported) == settings::toJson(settings::Settings{})) {
            setStatus(io::pathText(path.filename()) +
                          " has no preset in it, so nothing was changed.",
                      true);
            return false;
        }
    }

    // Q7's portable half only. The device, the MIDI clock port and the learned bindings are
    // this desk's and are deliberately untouched — see the header.
    setRules(loaded.preset.rules);
    meters_ = loaded.preset.meters;
    postOptions(loaded.preset.tempo);
    (void)runner_.postAndWait(output::OutputCommand::linkEnabled(loaded.preset.link));
    // **The lighting patch too** (M18): an import used to leave the patch alone, so every
    // imported lighting rule aimed at fixtures this rig did not have, and reached nothing.
    fixtures_ = loaded.preset.fixtures;
    runner_.post(output::OutputCommand::patch(fixtures_));
    patch_.setFixtures(fixtures_);
    editor_.setPatch(fixtures_);
    // The decoder and the OSC prefix are fixed for as long as the application runs — the
    // engine is built with one, and a receiver is configured for the other — so a preset that
    // carries different ones is kept for the next launch rather than dropped, and says so.
    std::string restart;
    if (loaded.preset.decoder != tracker_.engine().decoderKind()) {
        pendingDecoder_ = loaded.preset.decoder;
        restart += "the decoder";
    }
    if (output::isValidOscPrefix(loaded.preset.oscPrefix) &&
        loaded.preset.oscPrefix != runner_.snapshot().oscPrefix) {
        pendingPrefix_ = loaded.preset.oscPrefix;
        restart += restart.empty() ? "the OSC prefix " + loaded.preset.oscPrefix
                                   : " and the OSC prefix " + loaded.preset.oscPrefix;
    }

    // The rows the operator edits, not just the transports: these are the window's copy from
    // construction onwards (see the constructor), so an import that changed only the
    // transports would leave the boxes showing the old rig.
    targetDrafts_.clear();
    for (const output::OutputTarget& target : loaded.preset.outputs) {
        targetDrafts_.push_back(rowOf(target, midiPorts_));
    }
    publishTargetRows();
    applyTargets();

    setStatus("Imported " + io::pathText(path.filename()) + ": " +
                  std::to_string(loaded.preset.rules.size()) + " rules, " +
                  std::to_string(loaded.preset.outputs.size()) + " outputs, " +
                  std::to_string(loaded.preset.fixtures.size()) + " fixtures." +
                  (restart.empty() ? std::string{}
                                   : " Restart takt4 for " + restart + " to take effect."),
              false);
    return true;
}

void WindowController::publishOutputs() {
    // **A snapshot, not the live transports.** This runs on the redraw timer and the output
    // thread owns what it is reading: `outputs()` is a vector of strings that thread replaces
    // whole whenever a target changes, and `applyTargets` posts one of those and then calls
    // this — so on a running rig the two overlap by design, sixty times through a delay-slider
    // drag. Copying a vector while another thread reassigns it is a freed buffer, not a stale
    // number. `OutputRunner::snapshot` is taken under a lock and is at worst a millisecond old.
    const output::OutputRunner::Snapshot live = runner_.snapshot();
    window_->set_link_on(live.link);
    // Link's own, and safe to ask from any thread — as is the beat count, which is atomic.
    window_->set_link_peers(static_cast<int>(runner_.transports().link().numPeers()));

    // The rows themselves are **not** written here. They are what is being typed into, this
    // runs on the redraw timer, and a row replaced under the cursor is a row that cannot be
    // edited while the tracker runs. `applyTargets` owns them; see `targetDrafts_`.
    window_->set_osc_on(!live.outputs.empty());
    // The editor names these when it routes a rule, so it has to know what there is. Told
    // here rather than read from the runner, because this is the one place already holding a
    // safe copy.
    editor_.setTargets(live.outputs);

    window_->set_midi_port_index(
        deviceIndexOf(midiPorts_, live.midiClockPort.value_or(std::string{})));
    // Open is not the same as sending: a clock port whose device was pulled out is still open
    // as far as the transports know, and the indicator lit for it was the window claiming a
    // drum machine was hearing a clock it was not (the audit's H11a).
    const bool clockLost =
        live.midiClockPort &&
        std::find(live.lostMidi.begin(), live.lostMidi.end(), *live.midiClockPort) !=
            live.lostMidi.end();
    window_->set_midi_on(live.midiClockOpen && !clockLost);
    window_->set_beats_sent(static_cast<int>(runner_.transports().beats()));
    publishLostMidi(live.lostMidi);
    publishOutputProblems(live.outputProblems);
}

void WindowController::publishOutputProblems(const std::vector<std::string>& problems) {
    if (problems == outputProblemsShown_) {
        return;
    }
    const bool arrived = std::any_of(problems.begin(), problems.end(), [this](const auto& p) {
        return std::find(outputProblemsShown_.begin(), outputProblemsShown_.end(), p) ==
               outputProblemsShown_.end();
    });
    if (arrived) {
        // **Every output that cannot be reached, not only the one just found out.** One status
        // line, and a MIDI device left at home and a host that will not resolve are both things
        // the operator has to hear about — the second used to write the first away.
        std::string text;
        for (const std::string& problem : problems) {
            text += text.empty() ? "" : "; ";
            text += problem;
        }
        setStatus("outputs: " + text + ".", true);
    } else if (problems.empty() && !outputProblemsShown_.empty()) {
        setStatus("outputs: every output can be reached again.", false);
    }
    outputProblemsShown_ = problems;
}

void WindowController::publishLostMidi(const std::vector<std::string>& lost) {
    if (lost == lostMidiShown_) {
        return;
    }
    // Said when it changes, in words: a lighting desk or a sequencer that has stopped hearing
    // takt4 is otherwise invisible from here, and the output thread is already trying to bring
    // it back once a second.
    for (const std::string& name : lost) {
        if (std::find(lostMidiShown_.begin(), lostMidiShown_.end(), name) == lostMidiShown_.end()) {
            setStatus("MIDI device \"" + name +
                          "\" has stopped responding — unplugged? takt4 keeps trying to reopen it.",
                      true);
        }
    }
    for (const std::string& name : lostMidiShown_) {
        if (std::find(lost.begin(), lost.end(), name) == lost.end()) {
            setStatus("MIDI device \"" + name + "\" is back.", false);
        }
    }
    lostMidiShown_ = lost;
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
        // The version is not repeated here: it is in the title bar and the status bar's
        // corner now, and unlike this line neither of them is spent by the next status.
        setStatus("pick an input and press Start.", false);
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
    if (constructing_ && error) {
        startupErrors_.push_back(text); // shown together once the window is up; see there
    }
    statusIsError_ = error;
    // Through `shared`, and so through `io::validUtf8`: this is where exception messages land,
    // and those carry driver and device names in whatever encoding their library used.
    window_->set_status(shared(text));
    window_->set_status_is_error(error);
}

void WindowController::writeTickProbe() {
    if (std::ofstream probe(tickProbe_, std::ios::trunc); probe) {
        probe << static_cast<unsigned long long>(ticks_) << '\n';
    }
    // And a big, moving mark on screen, because the count in the file only says the round
    // happened — it says nothing about whether anything reached the screen, which is a
    // different claim and the one that was wrong. The input meter, swept, because it is wide
    // and unmistakable in a screenshot.
    window_->set_input_level(static_cast<float>(ticks_ % 60) / 60.0f);
    setStatus("tick " + std::to_string(ticks_), false);
}

void WindowController::pumpWhileDragged(void* self) {
    // Called from inside Windows' drag loop, on this same thread — see `native_window.hpp`.
    // One ordinary round plus the redraw the event loop would otherwise have asked for.
    auto* const controller = static_cast<WindowController*>(self);
    controller->tick();
    controller->window_->window().request_redraw();
    if (controller->editor_.visible()) {
        controller->editor_.window().window().request_redraw();
    }
}

void WindowController::tick() {
    ++ticks_;
    // First, and outside every widget callback: `publishTargetRows` found a row it could not
    // honestly update in place, so the repeater is built again here rather than from inside
    // the × that was pressed on the row being destroyed. One redraw later is 33 ms.
    if (targetRowsDirty_) {
        targetRowsDirty_ = false;
        targetModel_->clear();
        writeRows(*targetModel_, targetDrafts_);
    }
    if (!tickProbe_.empty()) {
        writeTickProbe();
    }
    // Half a second apart rather than every round: this walks the thread's top-level windows
    // and subclasses any it has not already, which is how the editor gets covered when it is
    // opened later. Doing it at all is what stops the whole window freezing while it is
    // dragged; doing it thirty times a second would be a list walk for nothing.
    if (ticks_ % 15 == 1) {
        keepPaintingWhileDragged(&WindowController::pumpWhileDragged, this);
    }
    // Before the early return: a MIDI message arrives on RtMidi's thread, so what it
    // changed — a learn that took, a control that was seen — only reaches the window on a
    // redraw, and binding buttons is something an operator does *before* pressing Start.
    // Unforced, so the port field is left alone unless the port itself has moved.
    publishControl(false);
    // And what the output thread made of the last change posted to it. **Here rather than at
    // the post**, because `post` is asynchronous while the tracker runs: the thread applies a
    // command about a millisecond later, so `lastError()` read straight after posting one
    // gives what the *previous* command left, which is empty. A MIDI device that is not on
    // this machine was therefore reported while the tracker was stopped and silently ignored
    // while it ran — the wrong way round, since a set is when it matters.
    if (const std::string error = runner_.lastError(); error != outputErrorShown_) {
        outputErrorShown_ = error;
        if (!error.empty()) {
            setStatus("outputs: " + error, true);
        }
    }
    // Above the early return with it, and for a related reason: what these two watch is
    // the engine's own state, which moves whether or not a device is open. A test drives
    // the engine directly without one, and holding the button lit forever there would be
    // the window lying about a snap that had in fact landed.
    publishSnap();
    // And the rules, for the same reason twice over: they fire on the output thread, and
    // the editor is where an operator watches them. Both above the early return — a rule
    // can be built and tested with no device open, which is exactly how one gets built.
    //
    // The editor first: it is what drains the fired log, and `publishTriggers` shows the
    // last message out of what the drain found. The other way round the TRIGGERS row was
    // always one redraw stale, which at 30 Hz nobody would see but which would be a lie.
    editor_.tick();
    // And the patch editor, whose live level bars are the one thing in that window that moves
    // on its own — and the only way to watch a fade happen with no fixture plugged in. It
    // costs nothing while the window is closed.
    patch_.tick();
    publishTriggers();
    // Above the early return too: a rig is mostly built with the tracker stopped, and that is
    // exactly the work a crash would otherwise take with it. See `enableAutosave`.
    if (!autosaveFile_.empty() && ticks_ % kAutosaveCheckTicks == 0) {
        autosave(nowSeconds());
    }
    // The input's health, above the early return because an outage is exactly when no stream
    // is open: the tracker is stopped between attempts to bring it back. See `superviseInput`.
    if (wantRunning_) {
        const double now = nowSeconds();
        audio::InputWatchdog::Reading reading;
        audio::AsioDriverEvents events;
        if (!outage_ && tracker_.stream() != nullptr) {
            reading = watchdog_.observe(tracker_.stream()->counters(), now);
            if (input_ && input_->device.hostApi == audio::HostApiKind::Asio) {
                events = audio::takeAsioDriverEvents();
            }
        }
        superviseInput(reading, events, now);
    }
    if (!tracker_.running()) {
        // The outputs keep going through an outage, so what they are doing is still shown.
        if (runner_.running()) {
            publishOutputs();
        }
        return;
    }

    // Every frame since the last tick joins the trace, newest at the right — every frame of
    // the network's, that is. A decoder running faster than the network is fed frames the
    // engine interpolates between the network's (`EngineFrame::interpolated`), and the
    // trace is a picture of what the network said, one column per 50 Hz frame; those are
    // skipped, and a beat that landed on one is carried to the next real column so it is
    // still drawn.
    bool moved = false;
    engine::EngineFrame frame;
    while (tracker_.engine().popFrame(frame)) {
        if (frame.interpolated) {
            traceBeatPending_ = traceBeatPending_ || frame.beat;
            continue;
        }
        std::rotate(trace_.begin(), trace_.begin() + 1, trace_.end());
        TracePoint point = tracePoint(frame);
        point.called = point.called || traceBeatPending_;
        traceBeatPending_ = false;
        trace_.back() = point;
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
