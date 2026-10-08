#include "ui/window_controller.hpp"

#include "core/assets/embedded.hpp"
#include "core/audio/asio_scan.hpp"
#include "core/audio/channel_picker.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/dmx/artnet_packet.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/engine/control.hpp"
#include "core/io/utf8.hpp"
#include "core/output/midi_ports.hpp"
#include "core/sandbox.hpp"
#include "ui/file_dialog.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
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
/// **Not the outputs** — Link and the MIDI clocks among them since 2026-09-25. An output can
/// name something that is not here today: a USB MIDI interface left at home, a media server whose
/// hostname does not resolve yet because the network is still coming up. `Transports` throws for
/// those, from inside `OutputRunner`'s constructor, from inside this class's member initialisers —
/// and nothing above that caught it, so takt4 died within seconds of every launch with no window
/// and no message (the audit's C1, reproduced with the Release build). The only way out was to
/// hand-edit `settings.json`. So both are applied once the window exists, through the same
/// `post` an operator's edit takes, and a failure lands on the status line like any other.
output::Transports::Config transportConfig(const settings::Settings& settings,
                                           const Options& tempo) {
    output::Transports::Config config;
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
/// Whether `part` ends in an Art-Net universe list so far — " u0" or " u0,1" as its last word —
/// so that a comma after it is the list's, not a new target. See `splitTargets`.
bool endsInUniverseList(std::string_view part) noexcept {
    const std::size_t space = part.find_last_of(" \t");
    if (space == std::string_view::npos || part.find("artnet") == std::string_view::npos) {
        return false;
    }
    const std::string_view last = part.substr(space + 1);
    if (last.size() < 2 || last[0] != 'u') {
        return false;
    }
    return last.substr(1).find_first_not_of("0123456789,") == std::string_view::npos;
}

/// Whether `part` begins with a plain number as its whole first word: the next universe of a
/// list that a comma split, with perhaps the target's delay and id after it.
bool startsWithUniverse(std::string_view part) noexcept {
    const std::size_t end = part.find_first_of(" \t");
    const std::string_view first = part.substr(0, end);
    return !first.empty() && first.find_first_not_of("0123456789") == std::string_view::npos;
}

/// The next comma or line break at or after `at` that is not inside a quoted name — the way a
/// name is written when it has one of those in it (`output::formatOutputTarget`, the audit of
/// 2026-09-25's L28). `npos` when there is none.
std::size_t nextSeparator(std::string_view text, std::size_t at) {
    bool quoted = false;
    for (std::size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted && c == '\\') {
            ++i; // the character after it is the name's, whatever it is
        } else if (c == '"') {
            quoted = !quoted;
        } else if (!quoted && (c == ',' || c == '\n')) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::vector<std::string_view> splitTargets(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t next = nextSeparator(text, at);
        const std::string_view part = trim(
            text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (!part.empty()) {
            // **A comma inside an Art-Net universe list is the list's.** `artnet h:6454 u0,1,4`
            // is one target, and splitting it at every comma left "1" and "4" as targets that
            // would not parse — so the whole row was refused and a node fed more than one
            // universe was never sent anything (found while fixing the audit's M21). Joined
            // back onto the target it belongs to, as the text it was: the views are into `text`.
            if (!parts.empty() && text[at - 1] == ',' && endsInUniverseList(parts.back()) &&
                startsWithUniverse(part)) {
                const std::size_t begin =
                    static_cast<std::size_t>(parts.back().data() - text.data());
                const std::size_t end = static_cast<std::size_t>(part.data() - text.data()) +
                                        part.size();
                parts.back() = text.substr(begin, end - begin);
            } else {
                parts.push_back(part);
            }
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
/// How soon after the last outage ended a new one goes on counting its tries rather than starting
/// from nothing: an input that is back for moments at a time is one outage, not many (M8).
constexpr double kOutageCarrySeconds = 30.0;
/// How long what the window met on the way up stays on the status line, joined by what it finds
/// out on its own, before anything may replace it (M13). Long enough to be read.
constexpr double kStartupHoldSeconds = 15.0;

/// START and STOP as the button presses them (the audit's M23; see
/// `WindowController::requestToggleRun`). Long enough for the window to draw "OPENING…" before
/// the driver takes the thread — a frame is 16 ms — and a press after it is carried out that
/// is refused, because a click made while the window could not draw arrives only then.
constexpr std::chrono::milliseconds kRunDrawFirst{40};
constexpr double kRunGraceSeconds = 0.3;

/// Where a row's MIDI dropdown sits for this device — an index into `ports`, the names behind the
/// window's `output-devices` after its entry 0, its own "not chosen yet" label.
///
/// `ports` is `WindowController::deviceNames_`: the MIDI outputs this machine has, and after them
/// every device a row names that it has not — a preset from another rig, an interface left at
/// home — marked in the list as not plugged in. So the row shows the device it is trying to open
/// rather than "select a MIDI device", which read as a row with no device at all (the audit of
/// 2026-09-25, L32). Zero only for a name in neither, which is a row still being filled in.
int deviceIndexOf(const std::vector<std::string>& ports, const std::string& device) {
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i] == device) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// `text` without `prefix` at its start, when it has one.
std::string withoutPrefix(std::string text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0 ? text.substr(prefix.size()) : text;
}

/// Why the MIDI control input would not open `port`, in the words its line shows — from inside
/// the `catch` that caught what opening it threw. Not on the machine and held by another program
/// are told apart, as they are for an output row: they have different fixes.
std::string midiControlProblem(const std::string& port) {
    try {
        throw;
    } catch (const output::MidiPortMissing&) {
        return "\"" + port + "\" is not on this machine \xE2\x80\x94 plug it in";
    } catch (const output::MidiPortBusy& e) {
        return e.reason();
    } catch (const std::exception& e) {
        return withoutPrefix(e.what(), "MIDI control: ");
    }
}

/// Why the OSC control socket would not bind, in the words its line shows: the port's reason
/// alone, without the "OSC control: cannot listen" the line already says by being red.
std::string oscControlProblem(const std::exception& e) {
    return withoutPrefix(withoutPrefix(e.what(), "OSC control: "), "cannot listen: ");
}

/// A target as the boxes of its row hold it.
///
/// The name box is left **empty** when the target is named after its own address, which is
/// what an unnamed one is called (`parseOutputTarget`). Filling it in with the address would
/// be true and useless: it is the box the operator types a name into, and it would come back
/// holding a copy of the box beside it every time they did not.
OutputRow rowOf(const output::OutputTarget& target, const std::vector<std::string>& midiPorts) {
    OutputRow row{};
    row.id = shared(target.id);
    const std::string address = output::formatOutputAddress(target);
    // Through `shared`: a MIDI target is named after its device unless somebody named it, and
    // the device's name is RtMidi's, in whatever encoding the driver gave it.
    row.name = shared(target.name == address ? std::string{} : target.name);
    row.address = shared(address);
    const bool device = target.kind == output::OutputTarget::Kind::Midi ||
                        target.kind == output::OutputTarget::Kind::MidiClock;
    row.kind_index = static_cast<int>(target.kind);
    row.host = shared(target.host);
    row.port = shared(std::to_string(target.port));
    row.device_index = device ? deviceIndexOf(midiPorts, target.device) : 0;
    row.enabled = target.enabled;
    row.sends_namespace = target.sendsNamespace;
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
    if (row.kind_index == static_cast<int>(output::OutputTarget::Kind::Link)) {
        return "link"; // there is nothing to ask: Link finds its peers itself
    }
    if (row.kind_index == static_cast<int>(output::OutputTarget::Kind::Midi) ||
        row.kind_index == static_cast<int>(output::OutputTarget::Kind::MidiClock)) {
        const auto device = static_cast<std::size_t>(row.device_index);
        if (row.device_index <= 0 || device > midiPorts.size()) {
            return {};
        }
        const bool clock =
            row.kind_index == static_cast<int>(output::OutputTarget::Kind::MidiClock);
        return (clock ? "midiclock " : "midi ") + midiPorts[device - 1];
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
    // Every node is fed every universe the patch uses since 2026-09-25: a node sends on the
    // universes it is set up for and ignores the rest, so there is no list to ask for.
    return artnet ? "artnet " + where : where;
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
        const bool device = target.kind == output::OutputTarget::Kind::Midi ||
                            target.kind == output::OutputTarget::Kind::MidiClock;
        const bool link = target.kind == output::OutputTarget::Kind::Link;
        row.kind_index = static_cast<int>(target.kind);
        row.host = shared(device || link ? std::string{} : target.host);
        row.port = shared(device || link ? std::string{} : std::to_string(target.port));
        row.device_index = device ? deviceIndexOf(midiPorts, target.device) : 0;
        return;
    }
    // Not a target — a row half-way through being typed, or one whose text was refused. The
    // kind is still readable from the shape of it, and for OSC so is as much of the host and
    // port as has been typed, which is what the boxes should go on showing.
    std::string_view text = trim(address);
    if (text.rfind("midiclock", 0) == 0) {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::MidiClock);
        return;
    }
    if (text.rfind("midi ", 0) == 0 || text == "midi") {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::Midi);
        return;
    }
    if (text.rfind("artnet ", 0) == 0 || text == "artnet") {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::ArtNet);
        text = trim(text.substr(text.size() > 6 ? 7 : 6));
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

/// "1 hop" or "12 hops", with what it means after it.
std::string counted(std::uint64_t n, const char* one, const char* many, const std::string& meaning) {
    return std::to_string(n) + " " + (n == 1 ? one : many) + " — " + meaning;
}

/// How many of `rules` would fire if their moment came, which is what "active" means to an
/// operator. A rule switched off and one that cannot fire are both not going to, and counting
/// them would make the TRIGGERS row lie in the direction that matters.
int activeRules(const std::vector<trigger::Rule::Config>& rules) {
    int active = 0;
    for (const trigger::Rule::Config& config : rules) {
        if (config.enabled && trigger::Rule(config).valid()) {
            ++active;
        }
    }
    return active;
}

/// The counters that are not zero, joined for one line of the window.
std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        out += out.empty() ? part : "  ·  " + part;
    }
    return out;
}

} // namespace

WindowController::WindowController(engine::LiveTracker& tracker)
    : WindowController(tracker, settings::Settings{}) {}

namespace {

settings::Settings withIds(settings::Settings settings) {
    // Link and the MIDI clock as outputs, as a file this build read would have them — settings
    // built some other way (a test, `takt4-shot`) may still hold them the old way.
    settings::migrateTransportOutputs(settings);
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
      peerModel_(std::make_shared<slint::VectorModel<LinkPeer>>()),
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
      patch_(runner_, settings.preset.fixtures, settings.preset.library) {
    // §4.3's stamp is taken on the audio thread, so the clock has to be installed before a
    // stream is opened. Handing it to the tracker rather than to the engine is what makes
    // that ordering `LiveTracker::start`'s business instead of this class's.
    tickProbe_ = environmentPath("TAKT4_TICK_PROBE");
    meters_ = settings.preset.meters;
    remembered_ = settings.machine;
    tracker_.setHostTimeSource(&runner_.hostTimeClock());
    midiPorts_ = output::listMidiOutputPorts();
    midiInputPorts_ = output::listMidiInputPorts();

    window_->set_trace(traceModel_);
    window_->set_outputs_list(targetModel_);
    window_->set_link_peer_list(peerModel_);
    // Set once and never again: the build does not change while it runs. It reaches the
    // title bar and the corner of the status bar, so "which build is this?" is answerable
    // at a glance and stays answerable — the opening status line used to be the only place
    // it was said, and the first status after it took the answer away.
    window_->set_version(slint::SharedString(versionLabel(buildInfo())));
    window_->set_version_short(
        slint::SharedString(shortVersionLabel(buildInfo().version, buildInfo().commit)));
    // The prefix an OSC row's tick names. Once: a new one takes a restart (`pendingPrefix_`).
    window_->set_osc_prefix(shared(usablePrefix(settings)));

    // §5.6's targets as the last run left them, into the rows that edit them. Seeded once:
    // the drafts are the window's copy from here on, because `publishOutputs` runs thirty
    // times a second while the tracker does and would otherwise replace a row mid-word.
    // From the settings rather than the runner, which has none yet — see `transportConfig`;
    // they reach it below, once there is a status line to report a failure on.
    (void)listDevicesFor(settings.preset.outputs);
    for (const output::OutputTarget& target : settings.preset.outputs) {
        targetDrafts_.push_back(rowOf(target, deviceNames_));
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
    window_->on_input_mono_toggled([this](bool mono) {
        deviceChosen_ = true;
        setMono(mono);
    });
    window_->on_toggle_run([this] { requestToggleRun(); });
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
    window_->on_latency_typed(
        [this](const slint::SharedString& text) { setLatencyTyped(std::string(text)); });
    window_->on_keep_shift_changed([this](bool keep) { setKeepShift(keep); });

    window_->on_link_peers_toggled([this] { toggleLinkPeers(); });
    window_->on_fold_clicked([this](int section) { toggleFold(section); });
    // A box's own commit counts only if it was typed into the list of outputs it now lands on —
    // see `outputsGeneration_`.
    window_->on_output_name_edited([this](int index, const slint::SharedString& name) {
        typedOutputs_ = outputsGeneration_;
        setTargetName(index, std::string(name), false);
    });
    window_->on_output_name_accepted([this](int index, const slint::SharedString& name) {
        if (typedOutputs_ == outputsGeneration_) {
            setTargetName(index, std::string(name), true);
        }
    });
    window_->on_output_host_edited([this](int index, const slint::SharedString& host) {
        typedOutputs_ = outputsGeneration_;
        setTargetHost(index, std::string(host), false);
    });
    window_->on_output_host_accepted([this](int index, const slint::SharedString& host) {
        if (typedOutputs_ == outputsGeneration_) {
            setTargetHost(index, std::string(host), true);
        }
    });
    window_->on_output_port_edited([this](int index, const slint::SharedString& port) {
        typedOutputs_ = outputsGeneration_;
        setTargetPort(index, std::string(port), false);
    });
    window_->on_output_port_accepted([this](int index, const slint::SharedString& port) {
        if (typedOutputs_ == outputsGeneration_) {
            setTargetPort(index, std::string(port), true);
        }
    });
    window_->on_output_kind_changed([this](int index, int kind) { setTargetKind(index, kind); });
    window_->on_output_device_picked(
        [this](int index, int device) { setTargetDevice(index, device); });
    window_->on_output_added([this] { addTarget(); });
    window_->on_save_now([this] { saveNow(); });
    window_->on_export_settings([this] {
        // The dialog runs its own message loop, so this must be the UI thread — which a
        // Slint callback is. An empty path is a cancel and `exportTo` does nothing with it.
        exportTo(askSaveFile("Export takt4 settings", "takt4-settings.json"));
    });
    window_->on_import_settings([this] { importFrom(askOpenFile("Import takt4 settings", "")); });
    // A double-click on × is one deletion: the row below moves up under the pointer and
    // would take the second click (`DeleteGuard`, the audit of 2026-09-25, L10).
    window_->on_output_removed([this](int index) {
        if (outputMarks_.press(index)) {
            removeTarget(index);
        }
    });
    window_->on_output_enabled_changed([this](int index, bool on) { setTargetEnabled(index, on); });
    window_->on_output_namespace_changed(
        [this](int index, bool on) { setTargetNamespace(index, on); });
    window_->on_outputs_namespace_all([this](bool on) { setAllNamespace(on); });
    window_->on_output_delay_changed([this](int index, float ms) { setTargetDelay(index, ms); });
    window_->on_output_delay_keyed([this](int) { typedOutputs_ = outputsGeneration_; });
    window_->on_output_delay_typed([this](int index, const slint::SharedString& text) {
        if (typedOutputs_ == outputsGeneration_) {
            setTargetDelayTyped(index, std::string(text));
        }
    });

    window_->on_midi_in_picked([this](int index) { pickMidiControlPort(index); });
    window_->on_learn_action_picked([this](int index) { pickLearnAction(index); });
    window_->on_learn_clicked([this] { toggleLearn(); });
    window_->on_forget_clicked([this] { forgetLearned(); });

    window_->on_osc_control_toggled([this](bool on) { setOscControlEnabled(on); });
    window_->on_osc_control_port_typed(
        [this](const slint::SharedString& text) { oscPortTyped_ = std::string(text); });
    window_->on_osc_control_port_edited([this](const slint::SharedString& text) {
        oscPortTyped_.reset();
        setOscControlPort(readPort(std::string(text)));
    });
    window_->on_osc_control_network_toggled([this](bool on) { setOscControlNetwork(on); });

    window_->on_rules_clicked([this] {
        openEditor();
        // With no rules the button reads "add a rule", so that is what it does: one added and
        // picked, ready to fill in. It used to open an empty editor, which added nothing.
        if (editor_.rules().empty()) {
            editor_.add();
        }
    });
    window_->on_about_opened([this] { openAbout(); });
    window_->on_fixtures_clicked([this] { patch_.show(); });
    window_->on_panic_clicked([this] { engagePanic(); });
    window_->on_panic_released([this] { releasePanic(); });

    // The editor owns the editing and this owns the file, so a change there comes back
    // here rather than the editor knowing where settings live.
    editor_.setRulesChanged([this](const std::vector<trigger::Rule::Config>& rules) {
        rules_ = rules;
        rulesActive_ = activeRules(rules_);
    });
    // The same for the patch — and one more thing: the rule editor's "lights" list shows every
    // fixture and group by name, so it has to be rebuilt whenever the patch changes or it will
    // go on offering a fixture under a name it no longer has. (A rule aims at a fixture by id,
    // so a rename moves no routing.)
    patch_.setPatchChanged([this](const std::vector<dmx::Fixture>& fixtures) {
        fixtures_ = fixtures;
        editor_.setPatch(fixtures_);
    });
    // The Liberation preset's zones, its Art-Net output and Link, which are this window's and the
    // patch editor's to hold — asked for by the rule editor before it adds the rules.
    editor_.setRigNeeded(
        [this](const RulesController::RigSetup& setup) { return applyRig(setup); });
    // The library an import adds to, or a re-import replaces, comes back the same way.
    patch_.setLibraryChanged(
        [this](const std::vector<fixtures::FixtureProfile>& library) { library_ = library; });

    publishControlLimits(*window_);
    window_->set_tap_needs(static_cast<int>(taps_.options().needTaps));

    publishPortLists();

    auto kinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    // In `output::OutputTarget::Kind`'s own order, which is what `OutputRow::kind-index` is.
    kinds->push_back(shared("OSC"));
    kinds->push_back(shared("MIDI"));
    kinds->push_back(shared("Art-Net"));
    kinds->push_back(shared("MIDI clock"));
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
    // A file from before there was a choice listened to one input: it is heard as the pair that
    // input is in now, and the operator is told so, once, with the way back (2026-09-28). Said
    // at the end, so nothing else the constructor says writes it away.
    std::string stereoNotice;
    if (settings.machine.stereoFromMono && !deviceFallback_ && selection().count == 2) {
        const audio::ChannelSelection pair = selection();
        stereoNotice = "Listening to In " + std::to_string(pair.channels[0] + 1) + " + " +
                       std::to_string(pair.channels[1] + 1) +
                       " as a stereo pair now: takt4 hears the average of the two, which tracks "
                       "as well as the better one alone and better on downbeats. Tick mono to go "
                       "back to In " +
                       std::to_string(settings.machine.channel + 1) + " alone.";
    }
    // The outputs, after the pickers, so one that has gone missing since the last run reports on
    // a status line the window already has rather than during construction. Posted whole,
    // exactly as they were saved: `Transports::setOutputs` opens every target it can and names
    // the ones it could not, so one missing MIDI interface or one hostname that does not resolve
    // yet costs that one output and says so, and the rest of the rig is sending. Link and every
    // MIDI clock are among them.
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
    library_ = settings.preset.library;
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
    // a relaunch finding the interface busy (the audit's H14). And the About box, which is a
    // window of its own as well and was left out (the audit of 2026-09-25, M16).
    window_->window().on_close_requested([this] {
        // What is being typed is finished first, as a click anywhere else finishes it: the
        // settings are saved once the loop has returned, and a name or a port typed and closed on
        // was not in them.
        applyDrafts();
        editor_.hide();
        patch_.hide();
        if (about_) {
            (*about_)->hide();
        }
        return slint::CloseRequestResponse::HideWindow;
    });

    // Before the first `show()`, which is what makes it stick — see `kMainWindowWidth`. The
    // markup's `preferred-width` does not size a Slint window; its content does, and with a
    // couple of output rows the content wanted more height than the window had, so the status
    // bar — which is where the version lives — was cut off the bottom.
    // And no taller than the screen has room for — see `fitToScreen` (the audit's M26).
    const LogicalExtent opening = fitToScreen({kMainWindowWidth, kMainWindowHeight});
    window_->window().set_size(slint::LogicalSize({opening.width, opening.height}));
    opening_ = {opening.width, opening.height};
    // Folded as it was left. The height that goes with it is `show`'s to work out.
    window_->set_inputs_folded(settings.machine.inputsFolded);
    window_->set_outputs_folded(settings.machine.outputsFolded);
    // And the rule editor's folds and its log, and the patch editor's folds, which are remembered
    // with these.
    editor_.applyLayout(settings.machine);
    patch_.applyLayout(settings.machine);

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
    //
    // **And one as well as several** (the audit of 2026-09-25, M13): a single error was left
    // where it was, and the constructor's own later news — "Control input on …", "pick an input
    // and press Start" — wrote over it before the window was up. The ASIO scan's problem, "N
    // rules will not fire" and a bad OSC prefix were each lost that way.
    constructing_ = false;
    if (!startupErrors_.empty()) {
        std::string all;
        for (const std::string& error : startupErrors_) {
            all += (all.empty() ? "" : "  ·  ") + error;
        }
        holdStartupMessage(all);
    }
    startupErrors_.clear();
    if (!stereoNotice.empty()) {
        report(stereoNotice, false); // joins the startup errors when there are any
    }

    timer_.start(slint::TimerMode::Repeated, kRedrawInterval, [this] { tick(); });
}

WindowController::~WindowController() {
    // Nothing may call back into this from Windows' timer once it starts to go.
    stopPumping(modalPump_);
    // Before any member goes: `runner_` owns the clock the audio thread reads on every hop,
    // and it is destroyed long before `tracker_`, which is not this class's. `ui::run` stops
    // the tracker itself before letting go of the window; a test that fails half-way through a
    // run does not, and that was a use-after-free on the audio thread.
    tracker_.stop();
    tracker_.setHostTimeSource(nullptr);
    tracker_.engine().setHostTimeSource(nullptr);
}

void WindowController::show() {
    window_->show();
    // The sections the settings left folded, folded again from the opening height exactly as a
    // click on each arrow folds them — so opening one gives back what its fold took here too.
    // Opened for a moment first, to measure what each fold takes; nothing is drawn before the
    // event loop runs.
    const bool inputs = window_->get_inputs_folded();
    const bool outputs = window_->get_outputs_folded();
    slint::Window& handle = window_->window();
    if ((!inputs && !outputs) || handle.is_maximized() || handle.is_fullscreen()) {
        return;
    }
    window_->set_inputs_folded(false);
    window_->set_outputs_folded(false);
    float height = opening_[1];
    if (inputs) {
        height = refold(0, true, opening_[0], height);
    }
    if (outputs) {
        height = refold(1, true, opening_[0], height);
    }
    if (height != opening_[1]) {
        handle.set_size(slint::LogicalSize({opening_[0], height}));
    }
}

void WindowController::run() {
    show();
    superviseThroughModalLoops();
    slint::run_event_loop();
    window_->hide();
    // Quiet before `ui::run` stops the tracker on the way out, as STOP is: that stop can wait on
    // the driver, and the outputs would go on firing the beats they predict meanwhile.
    if (wantRunning_) {
        runner_.setTracking(false);
    }
}

void WindowController::listDevices() {
    devices_ = tracker_.devices();
    auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const audio::InputDevice& device : devices_) {
        names->push_back(shared(describeDevice(device)));
    }
    window_->set_devices(names);
}

void WindowController::relistDuringOutage() {
    if (!input_) {
        return;
    }
    // The list PortAudio has just read, and **the device the outage is about found in it again
    // by name** (the audit of 2026-09-25, H1). This used to replace the list and keep the old
    // position — so the picker, STOP and START, and the settings file all meant whatever now sat
    // where the interface used to be.
    const audio::InputDevice wanted = input_->device;
    const int channel = input_->selection.channels.front();
    listDevices();
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].name == wanted.name && devices_[i].hostApiName == wanted.hostApiName) {
            window_->set_device_index(static_cast<int>(i));
            pickDevice(static_cast<int>(i));
            if (channel > 0 && channel < devices_[i].maxInputChannels) {
                selectChannel(channel);
            }
            return;
        }
    }
    // Not on the machine at the moment. Nothing else is selected in its place: START would open
    // that instead, and the settings would save it. It is still the one wanted — remembered as a
    // launch that could not find it remembers it (`deviceFallback_`), so it is what is saved and
    // what RESCAN looks for, and it is found again by name when it comes back.
    device_ = -1;
    window_->set_device_index(-1);
    window_->set_channels(std::make_shared<slint::VectorModel<slint::SharedString>>());
    remembered_.deviceName = wanted.name;
    remembered_.hostApiName = wanted.hostApiName;
    remembered_.channel = channel;
    deviceFallback_ = true;
    deviceChosen_ = false;
}

void WindowController::refreshDevices(const settings::MachineSettings& remembered) {
    mono_ = remembered.mono;
    listDevices();
    // Said whenever it is true, and last, so nothing below talks over it: an interface missing
    // from the list for a reason the operator cannot see would otherwise read as unplugged.
    const std::string asioProblem = audio::asioScanProblem();

    if (devices_.empty()) {
        // No input at all, so nothing is selected, and the remembered one is still the one
        // wanted: saved, and looked for by RESCAN. Left unset, the next save wrote no input and
        // forgot the interface that had only been switched off.
        deviceFallback_ = !remembered.deviceName.empty();
        device_ = -1;
        window_->set_channels(std::make_shared<slint::VectorModel<slint::SharedString>>());
        setStatus(asioProblem.empty()
                      ? "No input device. Connect an interface and press RESCAN."
                      : asioProblem,
                  true);
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
        selectChannel(remembered.channel);
    }
    if (!asioProblem.empty()) {
        setStatus(asioProblem, true);
    }
}

std::string WindowController::applyMachine(const settings::MachineSettings& machine) {
    std::string said;
    // **The input**, found by name as a launch finds it — and left alone when it is the one in
    // use, so importing this rig's own file interrupts nothing. Another one, while listening, is
    // listened to instead: stopped, switched, started again on it, as STOP, a pick and START would.
    const settings::MachineSettings now = currentSettings().machine;
    const bool sameInput = now.deviceName == machine.deviceName &&
                           now.hostApiName == machine.hostApiName &&
                           now.channel == machine.channel && now.mono == machine.mono;
    if (!sameInput) {
        const bool listening = wantRunning_ || tracker_.running();
        if (listening) {
            toggleRun();
        }
        // What is wanted from here on, as if takt4 had been launched with this file: an input
        // that is not on this machine falls back as a launch does and is still the one saved and
        // looked for (`deviceFallback_`), not the fallback.
        remembered_.deviceName = machine.deviceName;
        remembered_.hostApiName = machine.hostApiName;
        remembered_.channel = machine.channel;
        remembered_.mono = machine.mono;
        deviceChosen_ = false;
        refreshDevices(machine);
        // **And said**: an import that took the show off its input and put it on another — or on
        // none, the input not being on this machine — said only how many rules it brought.
        // Started again only on an input there is: with none at all — the list empty, the file
        // naming none — it says so rather than that it is listening to nothing.
        const bool picked = device_ >= 0 && static_cast<std::size_t>(device_) < devices_.size();
        if (listening && !deviceFallback_ && picked) {
            requestToggleRun();
            said = "Now listening to " + devices_[static_cast<std::size_t>(device_)].name +
                   ", as the file has it.";
        } else if (listening && deviceFallback_) {
            said = "Stopped listening: " + machine.deviceName +
                   " is not on this machine, so pick an input and press START.";
        } else if (listening) {
            said = "Stopped listening: there is no input. Connect an interface and press RESCAN.";
        }
    }

    // **MIDI control**: what was learned, then the port it was learned on — `setPort` keeps the
    // bindings it is given, and an empty port is no control input.
    std::vector<control::MidiBinding> bindings;
    for (const std::string& text : machine.midiBindings) {
        if (const std::optional<control::MidiBinding> binding = control::parseMidiBinding(text)) {
            bindings.push_back(*binding);
        }
    }
    control_.setBindings(std::move(bindings));
    if (machine.midiControlPort != control_.config().port) {
        setMidiControlPort(machine.midiControlPort);
    }

    // **OSC control**: switched off first when the file has it off, so the port and the network
    // are then only numbers; switched on last when it has it on, so it opens once, where the file
    // says.
    if (!machine.oscControlEnabled) {
        setOscControlEnabled(false);
    }
    setOscControlNetwork(!machine.oscControlLocalOnly);
    setOscControlPort(machine.oscControlPort);
    if (machine.oscControlEnabled) {
        setOscControlEnabled(true);
    }

    // **The layout**: the main window's folds, through the arrow's own path so the window's
    // height follows; and the rule editor's and the patch editor's.
    if (window_->get_inputs_folded() != machine.inputsFolded) {
        toggleFold(0);
    }
    if (window_->get_outputs_folded() != machine.outputsFolded) {
        toggleFold(1);
    }
    editor_.applyLayout(machine);
    patch_.applyLayout(machine);
    publishControl();
    return said;
}

void WindowController::publishPortLists() {
    // The first entry of each list is "nothing picked". **It says so in words**: it used to
    // be an empty string, and a dropdown showing nothing at all does not read as a list
    // nobody has chosen from — it reads as a box the application failed to fill in. Reported
    // from a rig about the control input, and the clock picker had the same hole.
    //
    // The MIDI outputs, as a MIDI row's and a MIDI clock row's dropdown offers them — the ones
    // this machine has, and after them any a row names that it has not, said to be missing
    // (`deviceNames_`, the audit of 2026-09-25's L32).
    auto devices = std::make_shared<slint::VectorModel<slint::SharedString>>();
    devices->push_back(
        shared(midiPorts_.empty() ? "no MIDI outputs on this machine" : "select a MIDI device"));
    for (std::size_t i = 0; i < deviceNames_.size(); ++i) {
        devices->push_back(shared(i < midiPorts_.size() ? deviceNames_[i]
                                                        : deviceNames_[i] + " \xE2\x80\x94 not plugged in"));
    }
    // A MIDI row's dropdown shows the entry its row names, from whichever list this is: it never
    // answers a new list by picking for itself, as std's ComboBox did (the audit of 2026-09-25,
    // L27, which had every MIDI row built again here).
    window_->set_output_devices(devices);

    publishMidiInputList();
}

std::vector<std::string> WindowController::midiInputChoices() const {
    std::vector<std::string> choices = midiInputPorts_;
    const std::string& wanted = control_.config().port;
    // By the rule the control input finds its port by, so a name saved from another numbering
    // of the same device is that device, not a second entry.
    if (!wanted.empty() && !output::findMidiPort(midiInputPorts_, wanted)) {
        choices.push_back(wanted);
    }
    return choices;
}

void WindowController::publishMidiInputList() {
    const std::vector<std::string> choices = midiInputChoices();
    auto inputs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    inputs->push_back(
        shared(midiInputPorts_.empty() ? "no MIDI inputs on this machine" : "select input"));
    for (std::size_t i = 0; i < choices.size(); ++i) {
        inputs->push_back(shared(i < midiInputPorts_.size()
                                     ? choices[i]
                                     : choices[i] + " \xE2\x80\x94 not plugged in"));
    }
    window_->set_midi_in_ports(inputs);
}

bool WindowController::listDevicesFor(const std::vector<output::OutputTarget>& targets) {
    std::vector<std::string> names = midiPorts_;
    for (const output::OutputTarget& target : targets) {
        const bool device = target.kind == output::OutputTarget::Kind::Midi ||
                            target.kind == output::OutputTarget::Kind::MidiClock;
        if (device && !target.device.empty() &&
            std::find(names.begin(), names.end(), target.device) == names.end()) {
            names.push_back(target.device);
        }
    }
    if (names == deviceNames_) {
        return false;
    }
    deviceNames_ = std::move(names);
    return true;
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
        keep.mono = mono_;
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
    (void)listDevicesFor(runner_.snapshot().outputs);
    publishPortLists();

    // Whatever was remembered and missing may be here now — a MIDI clock's device among the
    // outputs `applyTargets` reapplies below.
    if (!control_.running() && !control_.config().port.empty()) {
        setMidiControlPort(control_.config().port);
    }
    applyTargets();
    publishControl();
    // An ASIO scan that fell over again is said again, and last: pressing RESCAN is what that
    // message told the operator to do, and the steps above say things of their own over it.
    if (const std::string asioProblem = audio::asioScanProblem(); !asioProblem.empty()) {
        setStatus(asioProblem, true);
    } else if (!statusIsError_) {
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
    publishChannels();
    if (statusIsError_) {
        // Whatever went wrong was about the device that is no longer selected.
        setStatus("Pick an input and press Start.", false);
    }
}

namespace {

/// "In 11" — an input as the window names it in a sentence, without the driver's own name, which
/// can be long.
std::string inputName(int channel) {
    return "In " + std::to_string(channel + 1);
}

} // namespace

void WindowController::publishChannels() {
    window_->set_input_mono(mono_);
    auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
    if (device_ >= 0 && static_cast<std::size_t>(device_) < devices_.size()) {
        const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
        if (mono_ || device.maxInputChannels < 2) {
            for (int c = 0; c < device.maxInputChannels; ++c) {
                names->push_back(shared(describeChannel(device, c)));
            }
        } else {
            // The pairs an interface numbers its inputs in — 1 and 2, 3 and 4 — which is how a
            // stereo feed is patched into one. An odd one out at the end is offered alone.
            for (int c = 0; c < device.maxInputChannels; c += 2) {
                names->push_back(shared(c + 1 < device.maxInputChannels
                                            ? describePair(device, c)
                                            : describeChannel(device, c)));
            }
        }
    }
    window_->set_channels(names);
    selectChannel(channel_);
}

void WindowController::selectChannel(int channel) {
    channel_ = std::max(channel, 0);
    const bool pairs = !mono_ && device_ >= 0 &&
                       static_cast<std::size_t>(device_) < devices_.size() &&
                       devices_[static_cast<std::size_t>(device_)].maxInputChannels >= 2;
    window_->set_channel_index(pairs ? channel_ / 2 : channel_);
}

void WindowController::pickChannel(int index) {
    const bool pairs = !mono_ && device_ >= 0 &&
                       static_cast<std::size_t>(device_) < devices_.size() &&
                       devices_[static_cast<std::size_t>(device_)].maxInputChannels >= 2;
    channel_ = std::max(pairs ? index * 2 : index, 0);
}

void WindowController::setMono(bool mono) {
    if (mono == mono_) {
        window_->set_input_mono(mono_);
        return;
    }
    // The input stays: a pair's first alone, or the pair a single input is in.
    mono_ = mono;
    publishChannels();
}

audio::ChannelSelection WindowController::selection() const {
    if (device_ < 0 || static_cast<std::size_t>(device_) >= devices_.size()) {
        return audio::ChannelSelection::single(channel_);
    }
    const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
    const int first = channel_ - channel_ % 2;
    if (mono_ || first + 1 >= device.maxInputChannels) {
        return audio::ChannelSelection::single(channel_);
    }
    return audio::ChannelSelection::pair(first, first + 1);
}

void WindowController::superviseStereo(const audio::StereoSums& sums, double now) {
    const audio::StereoCheck::Reading reading = stereoCheck_.observe(sums, now);
    std::string problem;
    if (input_ && input_->selection.count == 2) {
        problem = audio::StereoCheck::describe(reading.verdict,
                                               inputName(input_->selection.channels[0]),
                                               inputName(input_->selection.channels[1]));
    }
    if (problem == stereoProblem_) {
        return;
    }
    // On the status line when it starts, under the meter for as long as it lasts (the input's
    // trouble line, which `superviseInput` writes).
    if (!problem.empty()) {
        report(problem + ".", true);
    } else {
        // **And off the status line when it ends.** It stayed there, amber, until something else
        // was said, so a pair that had been put right went on reading "out of phase" until its
        // input was stopped and started again (the operator, 2026-10-08). Only its own sentence
        // goes: a status line that has said something since is the operator's news.
        const std::string sentence = stereoProblem_ + ".";
        if (const std::size_t at = startupMessage_.find(sentence); at != std::string::npos) {
            // Joined to what the window met starting (`report`): taken back out of it.
            const std::string joint = "  \xC2\xB7  ";
            std::size_t from = at;
            std::size_t length = sentence.size();
            if (from >= joint.size() && startupMessage_.compare(from - joint.size(), joint.size(),
                                                                joint) == 0) {
                from -= joint.size();
                length += joint.size();
            } else if (startupMessage_.compare(at + length, joint.size(), joint) == 0) {
                length += joint.size();
            }
            startupMessage_.erase(from, length);
            showStatus(startupMessage_, !startupMessage_.empty());
        } else if (std::string(window_->get_status()) == sentence) {
            setStatus(input_ && input_->selection.count == 2
                          ? inputName(input_->selection.channels[0]) + " and " +
                                inputName(input_->selection.channels[1]) + " are fine again."
                          : std::string(),
                      false);
        }
    }
    stereoProblem_ = problem;
}

void WindowController::requestToggleRun() {
    // The audit's M23. Opening an ASIO driver, and closing one, holds this thread for as long as
    // the driver likes — seconds, on some — and nothing is drawn meanwhile, so the button went on
    // saying START with no sign it had been pressed. A click made in that time is delivered once
    // the thread is free, and pressed STOP on an input that had only just opened. So: say it on
    // the button, give the window a frame to show it, carry it out, and take no press until a
    // moment after — the button is disabled for all of it.
    if (runPending_ || window_->get_run_busy()) {
        return;
    }
    const bool stopping = wantRunning_ || tracker_.running();
    runPending_ = true;
    window_->set_run_busy(true);
    window_->set_run_busy_text(shared(stopping ? "STOPPING\xE2\x80\xA6" : "OPENING\xE2\x80\xA6"));
    runTimer_.start(slint::TimerMode::SingleShot, kRunDrawFirst, [this] {
        toggleRun();
        runPending_ = false;
        runSettledAt_ = nowSeconds();
        window_->set_run_busy_text(shared(""));
    });
}

void WindowController::toggleRun() {
    // What an import said, for the START it asked for and no other: one that fails says why
    // instead, and the next one says what it opened. See `importFrom`.
    const std::string importSaid = std::exchange(importSaid_, std::string{});
    // By what was asked for, not by whether a stream is open: during an outage the tracker is
    // stopped between attempts to reopen it, and STOP has to mean stop — not "start".
    if (wantRunning_ || tracker_.running()) {
        wantRunning_ = false;
        outage_.reset();
        outageEndedAt_ = -1.0;
        nothingPlaying_ = false;
        input_.reset();
        stereoProblem_.clear();
        window_->set_input_lost(false);
        window_->set_input_trouble(shared(""));
        // **The runner first**, then the tracker. The runner goes on — it runs for the
        // application's whole life — and is told the tracker is stopping: nothing fires after
        // that, not the beats the tracker calls on its way down nor any it would have predicted,
        // what is owed to a clip or a laser goes out at once, the MIDI clock stops, and the lights
        // go out. The other way round, the runner heard only once the tracker had stopped — which
        // waits on the driver — and went on firing the beats it predicted meanwhile.
        runner_.setTracking(false);
        tracker_.stop();
        publishStopped();
        return;
    }
    // Nothing selected — the machine has no inputs, or the one being used went away and nothing
    // was put in its place — or a position the list no longer has, which used to be read anyway
    // (the audit of 2026-09-25, H1).
    if (device_ < 0 || static_cast<std::size_t>(device_) >= devices_.size()) {
        setStatus(devices_.empty() ? "No input device. Connect an interface and press RESCAN."
                                   : "Pick an input, then press Start.",
                  true);
        publishStopped();
        return;
    }
    const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
    try {
        tracker_.start(device, selection());
    } catch (const audio::DriverNotAnswering& e) {
        // Not the busy-interface advice below: nothing says another program has it, and the way
        // round is time, or a restart — see `publishDriverState`.
        setStatus("Cannot open " + device.name + ": " + e.what() + ".", true);
        publishStopped();
        return;
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
    // After the tracker, so the MIDI clock does not tick for a run that failed to open.
    runner_.setTracking(true);
    wantRunning_ = true;
    nothingPlaying_ = false;
    input_ = tracker_.current();
    watchOpenedInput();
    window_->set_running(true);
    publishOpenStream(importSaid);
}

void WindowController::watchOpenedInput() {
    watchdog_.reset(tracker_.stream()->sampleRate(), nowSeconds());
    stereoCheck_.reset();
    stereoProblem_.clear();
    // What the driver said while it was being opened belongs to that open, not to the stream it
    // made. Left for the next redraw, a driver that says anything as it starts was answered with
    // another reopen, and so on every tick (the audit of 2026-09-25, L22).
    (void)audio::takeAsioDriverEvents();
    driverResyncs_ = 0;
    inputTroubleShown_ = {};
    window_->set_input_trouble(shared(""));
}

bool WindowController::reopenInput(std::string& error) {
    if (!input_) {
        error = "nothing was open";
        return false;
    }
    tracker_.stop();
    // Found again by name: a rescan renumbers every device, and the index the input had when
    // it was opened may now belong to something else.
    const audio::InputDevice* device = nullptr;
    for (const audio::InputDevice& candidate : devices_) {
        if (candidate.name == input_->device.name &&
            candidate.hostApiName == input_->device.hostApiName) {
            device = &candidate;
            break;
        }
    }
    if (device == nullptr) {
        // **Not there is a failure, not a reason to use the old entry** (the audit of
        // 2026-09-25, H1). The old one's index belongs to the PortAudio session before the
        // rescan, and opening it opened whatever now has that number — and said "Audio is back".
        error = input_->device.name + " is not on this machine at the moment";
        return false;
    }
    try {
        tracker_.start(*device, input_->selection);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    input_ = tracker_.current();
    watchOpenedInput();
    return true;
}

void WindowController::beginOutageOn(const engine::LiveTracker::Running& input, double now) {
    tracker_.stop();
    wantRunning_ = true;
    input_ = input;
    window_->set_running(true);
    beginOutage("No audio from " + input.device.name, now, now);
}

void WindowController::assumeRunningOn(const engine::LiveTracker::Running& input, double now) {
    wantRunning_ = true;
    input_ = input;
    outage_.reset();
    nothingPlaying_ = false;
    watchdog_.reset(input.device.defaultSampleRate, now);
    driverResyncs_ = 0;
    window_->set_running(true);
}

void WindowController::beginOutage(const std::string& why, double since, double now) {
    Outage outage;
    outage.since = since;
    outage.nextTry = now + kOutageFirstTrySeconds;
    outage.why = why;
    // An input that came back for a moment and went again goes on counting, so the rescan every
    // third try is still reached (the audit of 2026-09-25, M8): each return used to start the
    // count from nothing, and an input that kept flapping never had the machine looked at again.
    if (outageEndedAt_ >= 0.0 && now - outageEndedAt_ < kOutageCarrySeconds) {
        outage.tries = outageEndedTries_;
    }
    outage_ = outage;
    // And the outputs, which let go of the rig as they do when the input goes quiet: an
    // interface unplugged sends nothing at all, so the tracker never hears the silence that says
    // so, and the lasers held their last clip and the notes stayed on through the outage.
    runner_.post(output::OutputCommand::inputLost());
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
        setStatus(why + "; reopened at " + fixed(stream->sampleRate(), 0) + " Hz.\n" +
                      latencyLine(),
                  false);
        return;
    }
    beginOutage(why + ", and it would not open again (" + error + ")", now, now);
}

void WindowController::publishDriverState() {
    const bool stuck = tracker_.stuck();
    if (stuck == driverStuckShown_) {
        return;
    }
    driverStuckShown_ = stuck;
    if (stuck) {
        // **Said, and gone on from** (the audit of 2026-09-25, L23): the window used to be frozen
        // by now, PANIC and all. The stream was detached before it was handed over, so nothing
        // the driver sends reaches the tracker; the outputs carry the last tempo on.
        const std::string asked = tracker_.stuckOn();
        report("The audio driver has stopped answering (it was asked to " + asked +
                   "). takt4 has let it go and carries on \xE2\x80\x94 the outputs keep the last "
                   "tempo \xE2\x80\x94 but cannot open an input until the driver answers. If it "
                   "does not, restart takt4.",
               true);
        window_->set_input_trouble(shared("audio driver not answering since it was asked to " +
                                          asked + " \xE2\x80\x94 restart takt4 if it does not "
                                                  "come back"));
    } else {
        report("The audio driver is answering again.", false);
        window_->set_input_trouble(shared(""));
    }
}

void WindowController::superviseInput(const audio::InputWatchdog::Reading& reading,
                                      const audio::AsioDriverEvents& events, double now) {
    if (!wantRunning_) {
        return;
    }
    const std::string name = input_ ? input_->device.name : std::string("the input");

    if (outage_) {
        // **Back when the reopened input sends something, not when it opens** (the audit of
        // 2026-09-25, M8). A reopen that merely opened was announced as "Audio is back" and
        // ended the outage before a single callback, so an input that opens and then sends
        // nothing — a loopback of an idle output is exactly that — flipped between "No audio"
        // and "Audio is back" every few seconds, restarting the tracker each time.
        if (outage_->reopened) {
            const audio::InputStream* stream = tracker_.stream();
            if (stream != nullptr && stream->counters().callbacks > 0) {
                const double lasted = now - outage_->since;
                outageEndedAt_ = now;
                outageEndedTries_ = outage_->tries;
                outage_.reset();
                window_->set_input_lost(false);
                setStatus("Audio is back from " + name + " after " + fixed(lasted, 0) +
                              " s without it; reopened at " + fixed(stream->sampleRate(), 0) +
                              " Hz.\n" + latencyLine(),
                          false); // fixed, so not red — see `restartInput`
                return;
            }
            if (now < outage_->nextTry) {
                return;
            }
            // Open, and nothing came: that was a try. Closed again for the next.
            tracker_.stop();
            outage_->reopened = false;
        } else if (now < outage_->nextTry) {
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
            const double scanFrom = nowSeconds();
            tracker_.stop();
            try {
                if (tracker_.rescan()) {
                    relistDuringOutage();
                }
            } catch (const std::exception&) {
                // PortAudio would not come back up; the next try will ask again.
            }
            // When the look ended, on the clock `now` is on. An ASIO driver can take the scan's
            // whole limit to answer, and timed from when it began the next look was due that
            // much sooner (the audit of 2026-09-25, L24).
            rescannedAt_ = now + (nowSeconds() - scanFrom);
        }
        std::string error;
        if (reopenInput(error)) {
            outage_->reopened = true;
            outage_->nextTry = now + kOutageRetrySeconds;
            setStatus(outage_->why + " — reopened after " + fixed(now - outage_->since, 0) +
                          " s, waiting for it to send audio...",
                      true);
        } else {
            outage_->nextTry = now + kOutageRetrySeconds;
            setStatus(outage_->why + " — still no audio after " +
                          fixed(now - outage_->since, 0) + " s (" + error + "). Trying again...",
                      true);
        }
        return;
    }

    // What the ASIO driver said. Each of these means the stream as opened no longer describes
    // the hardware, and PortAudio's host used to acknowledge them and carry on regardless —
    // except a "new rate" that is the rate the stream already runs at. Drivers send that for
    // other news (the SDK's comment names S/PDIF status), and it was answered with a reopen
    // every time (the audit of 2026-09-25, L22).
    const double runningAt = watchdog_.openedRate();
    const bool sameRate = events.sampleRateChange && !events.resetRequest &&
                          !events.bufferSizeChange && runningAt > 0.0 &&
                          std::abs(events.reportedRate / runningAt - 1.0) < 0.001;
    if (events.needsReopen() && !sameRate) {
        const char* what = events.resetRequest       ? "asked to be reset"
                           : events.sampleRateChange ? "changed its sample rate"
                                                     : "changed its buffer size";
        restartInput("The driver for " + name + " " + what, now);
        return;
    }

    // The interface's overflows, and what the engine lost after them — the audit's M12: the
    // engine counted every hop, frame and beat it dropped and nothing showed one. All of them
    // start again with the run, like the overflows.
    const engine::BeatEngine& engine = tracker_.engine();
    InputTrouble trouble;
    trouble.overflows = reading.inputOverflows;
    // The driver's own word that it lost its place for a moment, kept "so it can be shown" and
    // shown nowhere until the audit of 2026-09-25 (L26).
    if (events.resync) {
        ++driverResyncs_;
    }
    trouble.resyncs = driverResyncs_;
    trouble.hopsDropped = engine.activations().hopsDropped();
    trouble.framesDropped = engine.activations().framesDropped();
    trouble.beatsDropped = engine.beatsDropped();
    // Repaired where the stream picks its channel, and so never seen by the engine, whose own
    // repair is for audio that did not come through a stream.
    trouble.samplesRepaired =
        engine.activations().samplesRepaired() +
        (tracker_.stream() != nullptr ? tracker_.stream()->counters().samplesRepaired : 0);
    trouble.stereo = stereoProblem_;
    if (trouble != inputTroubleShown_) {
        inputTroubleShown_ = trouble;
        std::vector<std::string> parts;
        if (!trouble.stereo.empty()) {
            parts.push_back(trouble.stereo); // first: the one an operator can fix at the desk
        }
        if (trouble.overflows != 0) {
            parts.push_back(counted(trouble.overflows, "input overflow", "input overflows",
                                    "the interface dropped audio"));
        }
        if (trouble.hopsDropped != 0) {
            parts.push_back(counted(trouble.hopsDropped, "hop dropped", "hops dropped",
                                    "the model fell behind the audio"));
        }
        if (trouble.framesDropped != 0) {
            parts.push_back(counted(trouble.framesDropped, "frame dropped", "frames dropped",
                                    "the tracker fell behind the model"));
        }
        if (trouble.beatsDropped != 0) {
            parts.push_back(counted(trouble.beatsDropped, "beat dropped", "beats dropped",
                                    "the outputs fell behind the tracker"));
        }
        if (trouble.resyncs != 0) {
            parts.push_back(counted(trouble.resyncs, "driver resync", "driver resyncs",
                                    "the interface lost its place for a moment"));
        }
        if (trouble.samplesRepaired != 0) {
            parts.push_back(counted(trouble.samplesRepaired, "sample", "samples",
                                    "not a number, heard as silence"));
        }
        window_->set_input_trouble(shared(joined(parts)));
    }

    // A loopback that had gone quiet and is sending again.
    if (nothingPlaying_ && reading.verdict == audio::InputWatchdog::Verdict::Healthy) {
        nothingPlaying_ = false;
        setStatus("Sound on " + name + " again.", false);
    }

    switch (reading.verdict) {
    case audio::InputWatchdog::Verdict::Silent:
        // **A loopback's silence is nothing playing, not a dead input** (the audit of 2026-09-25,
        // M8, and the operator's call on its Q4). Windows sends a loopback nothing at all while
        // nothing plays on the output it captures — measured: 0 hops in 3 s from an idle output's
        // loopback — so a paused player read as an unplugged interface, and was reopened every
        // few seconds, the tracker restarting each time. Said once, and the stream left open for
        // whatever plays next.
        if (input_ && input_->device.isLoopback) {
            if (!nothingPlaying_) {
                nothingPlaying_ = true;
                peak_ = 0.0f;
                window_->set_input_level(0.0f);
                window_->set_input_peak(0.0f);
                window_->set_input_reading(shared("nothing playing"));
                setStatus("Nothing is playing on " + name +
                              ". takt4 is listening, and follows it as soon as something plays.",
                          false);
            }
            break;
        }
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

void WindowController::toggleLinkPeers() {
    if (linkPeersShown_) {
        linkPeersShown_ = false;
        linkPeers_.close();
        peerModel_->clear();
    } else {
        std::string problem;
        if (!linkPeers_.open(problem)) {
            // On the Link row as well as the status line, until the list is asked for again or
            // opens: a port that would not bind is said where the button that needed it is.
            linkPeersProblem_ = "cannot list peers: " + problem;
            setStatus("Link peers: " + problem, true);
            publishOutputs();
            return;
        }
        linkPeersShown_ = true;
    }
    linkPeersProblem_.clear();
    window_->set_link_peers_shown(linkPeersShown_);
    publishLinkPeers();
}

void WindowController::toggleFold(int section) {
    if (section != 0 && section != 1) {
        return;
    }
    // What is typed into an output row is applied first, as every action here does (see
    // `applyDrafts`): the click on the arrow took the keyboard from the box, and the box's own
    // commit comes a turn of the event loop later.
    applyDrafts();
    const auto at = static_cast<std::size_t>(section);
    const bool folding =
        section == 0 ? !window_->get_inputs_folded() : !window_->get_outputs_folded();
    slint::Window& handle = window_->window();
    if (handle.is_maximized() || handle.is_fullscreen()) {
        if (section == 0) {
            window_->set_inputs_folded(folding);
        } else {
            window_->set_outputs_folded(folding);
        }
        foldTaken_[at] = 0.0f; // the operator's size, not this class's to change
        return;
    }
    const float width = window_->get_shown_width();
    const float height = window_->get_shown_height();
    const float next = refold(at, folding, width, height);
    if (next != height) {
        handle.set_size(slint::LogicalSize({width, next}));
    }
}

float WindowController::refold(std::size_t at, bool folding, float width, float height) {
    // What the fold takes off the content, or gives back: its least height either side of the
    // change. Read straight after setting the property, since Slint works a layout out when it is
    // read — which is also what makes `fold-keep` below the new layout's and not the old one's.
    const float before = window_->get_content_least();
    if (at == 0) {
        window_->set_inputs_folded(folding);
    } else {
        window_->set_outputs_folded(folding);
    }
    const float after = window_->get_content_least();
    if (folding) {
        // Shorter by what was folded — but the lowest folded heading, and the arrow that opens it
        // again, stays in sight above what is pinned, and the window keeps its own minimum.
        const float keep = window_->get_fold_keep() + window_->get_pinned_height();
        const float next =
            std::min(height, std::max({kMainWindowMinHeight, height - (before - after), keep}));
        foldTaken_[at] = height - next;
        return next;
    }
    // What the fold took, given back — no taller than the screen has room for.
    const float next = fitToScreen({width, height + foldTaken_[at]}).height;
    foldTaken_[at] = 0.0f;
    return next;
}

void WindowController::publishLinkPeers() {
    if (!linkPeersShown_) {
        return;
    }
    linkPeers_.poll(nowSeconds());
    std::vector<LinkPeer> rows;
    for (const output::LinkPeer& peer : linkPeers_.peers()) {
        std::string addresses;
        for (const std::string& address : peer.addresses) {
            addresses += (addresses.empty() ? "" : ", ") + address;
        }
        rows.push_back(LinkPeer{shared(addresses),
                                shared(peer.bpm > 0.0 ? fixed(peer.bpm, 2) + " BPM" : "—"),
                                peer.sameSession,
                                shared(!peer.playing   ? ""
                                       : *peer.playing ? "playing"
                                                       : "stopped")});
    }
    // In place, so a list that has not changed is not rebuilt thirty times a second.
    writeRows(*peerModel_, rows);
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
    splitAddress(row, deviceNames_);
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
    } else {
        draftsPending_ = true;
    }
}

void WindowController::setTargetHost(int index, const std::string& host, bool apply) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    row.host = shared(host);
    row.address = shared(addressOf(row, deviceNames_));
    if (apply) {
        applyTargets();
    } else {
        draftsPending_ = true;
    }
}

void WindowController::setTargetPort(int index, const std::string& port, bool apply) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    row.port = shared(port);
    row.address = shared(addressOf(row, deviceNames_));
    if (apply) {
        applyTargets();
    } else {
        draftsPending_ = true;
    }
}

void WindowController::setTargetKind(int index, int kind) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    OutputRow& row = targetDrafts_[static_cast<std::size_t>(index)];
    // Link is not a kind a row can be switched to or from: there is one, always first.
    constexpr int link = static_cast<int>(output::OutputTarget::Kind::Link);
    if (row.kind_index == link || kind < 0 || kind >= link) {
        return;
    }
    if (row.kind_index == kind) {
        return; // the kind the row already is: nothing changes
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
        row.port = shared(std::to_string(newTargetPort_));
    }
    // A row switched to MIDI has no device picked yet, and one switched back to OSC keeps
    // whatever host and port it had — so `addressOf` gives an empty destination for the
    // first and the old one back for the second. Empty is how `applyTargets` spells "still
    // being filled in", so switching kind never raises an error about an unfinished row.
    row.address = shared(addressOf(row, deviceNames_));
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
    // A device picked on a row that asks for one — MIDI or a MIDI clock — and made MIDI when
    // it did not, which is the only other kind with a device.
    if (row.kind_index != static_cast<int>(output::OutputTarget::Kind::MidiClock)) {
        row.kind_index = static_cast<int>(output::OutputTarget::Kind::Midi);
    }
    row.address = shared(addressOf(row, deviceNames_));
    applyTargets();
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
    row.port = shared(std::to_string(newTargetPort_));
    row.address = shared(addressOf(row, deviceNames_));
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

std::string WindowController::applyRig(const RulesController::RigSetup& setup) {
    // **A fixture import half done in the patch editor is dropped by a new patch** — said first,
    // and ADD pressed again goes on. It went without a word.
    if (patch_.importing() && !setup.goAhead) {
        return "Adding drops the fixture import half done in the patch editor. Finish it there "
               "first, or add again to drop it.";
    }
    // The patch, as an import lays one: the runner, the patch editor and the rule editor.
    fixtures_ = setup.patch;
    runner_.post(output::OutputCommand::patch(fixtures_));
    patch_.setFixtures(fixtures_, library_);
    editor_.setPatch(fixtures_);

    // An Art-Net output to Liberation — one already there, switched on, or a new one — and Link,
    // which Liberation's tempo follows. Whatever is being typed into the outputs goes first.
    applyDrafts();
    constexpr int artNet = static_cast<int>(output::OutputTarget::Kind::ArtNet);
    constexpr int link = static_cast<int>(output::OutputTarget::Kind::Link);
    bool found = false;
    for (OutputRow& row : targetDrafts_) {
        if (row.kind_index == artNet && std::string(row.host) == setup.artNetHost &&
            readPort(std::string(row.port)) == static_cast<int>(setup.artNetPort)) {
            row.enabled = true;
            found = true;
        }
        if (row.kind_index == link) {
            row.enabled = true;
        }
    }
    if (!found) {
        OutputRow row{};
        row.enabled = true;
        row.kind_index = artNet;
        row.name = shared("Liberation");
        row.host = shared(setup.artNetHost);
        row.port = shared(std::to_string(setup.artNetPort));
        row.address = shared(addressOf(row, deviceNames_));
        std::vector<output::OutputTarget> known = runner_.snapshot().outputs;
        for (const OutputRow& other : targetDrafts_) {
            output::OutputTarget holder;
            holder.id = std::string(other.id);
            known.push_back(std::move(holder));
        }
        row.id = shared(output::newOutputId(known));
        targetDrafts_.push_back(row);
    }
    applyTargets();
    return {};
}

void WindowController::removeTarget(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size() ||
        targetDrafts_[static_cast<std::size_t>(index)].kind_index ==
            static_cast<int>(output::OutputTarget::Kind::Link)) {
        return; // the Link row is switched off, never removed
    }
    const auto at = static_cast<std::size_t>(index);
    targetDrafts_.erase(targetDrafts_.begin() + index);
    // **That row's element, and no other** (the audit of 2026-09-25, M15). Left to `writeRows`,
    // the list was rewritten in place and the *last* element went — so every element below this
    // one was handed the output below it. A box being typed into on one of them commits when it
    // loses the keyboard, a turn of the event loop after this click, by its index: into the next
    // output down. Erased here, each element goes on holding its own output, its index moves up
    // with it, and what it was typed for is what it commits to.
    if (at < targetModel_->row_count()) {
        targetModel_->erase(at);
    }
    applyTargets();
}

void WindowController::setTargetEnabled(int index, bool on) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    targetDrafts_[static_cast<std::size_t>(index)].enabled = on;
    applyTargets();
}

void WindowController::setTargetNamespace(int index, bool on) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    targetDrafts_[static_cast<std::size_t>(index)].sends_namespace = on;
    applyTargets();
}

void WindowController::setAllNamespace(bool on) {
    for (OutputRow& row : targetDrafts_) {
        if (row.kind_index == static_cast<int>(output::OutputTarget::Kind::Osc)) {
            row.sends_namespace = on;
        }
    }
    applyTargets();
}

void WindowController::applyDrafts() {
    if (draftsPending_) {
        applyTargets();
    }
    // The OSC control port's box, which commits only when finished with — a socket reopened on
    // every digit would be no use — and so was not finished by anything but itself.
    if (oscPortTyped_) {
        const std::string typed = std::move(*oscPortTyped_);
        oscPortTyped_.reset();
        setOscControlPort(readPort(typed));
    }
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
    // **Only with nothing else waiting.** A name, a host or a port typed and not yet committed
    // went into the rows with the delay (`publishTargetRows`), so the box's own commit, a turn
    // later, found nothing changed — and what was typed was never applied. With such a draft
    // waiting, the whole list goes, this delay with it; the drag after it is the fast path again.
    if (!draftsPending_ && !id.empty() &&
        output::findTarget(runner_.snapshot().outputs, id) != nullptr) {
        runner_.post(output::OutputCommand::outputDelay(id, static_cast<double>(clamped) / 1000.0));
        publishTargetRows();
        publishOutputs();
        return;
    }
    applyTargets();
}

void WindowController::setTargetDelayTyped(int index, const std::string& text) {
    if (index < 0 || static_cast<std::size_t>(index) >= targetDrafts_.size()) {
        return;
    }
    const std::optional<double> ms = readMilliseconds(text);
    if (!ms) {
        setStatus("\"" + text + "\" is not a number of milliseconds, so the delay was left at " +
                      std::to_string(static_cast<int>(
                          std::lround(targetDrafts_[static_cast<std::size_t>(index)].delay_ms))) +
                      " ms.",
                  true);
        return;
    }
    setTargetDelay(index, static_cast<float>(*ms));
    // The same row again even when nothing moved, so a number typed past the end of the range
    // comes back as the end of the range rather than as what was typed.
    publishTargetRows();
}

void WindowController::setOscTargets(const std::string& text) {
    // The whole rig as one piece of text, which is what a settings file's line looks like
    // and what somebody pastes. It arrives as a single draft and `applyTargets` splits it
    // into rows, which is the same path a pasted row takes — after the Link row, which a line
    // of destinations says nothing about.
    std::vector<OutputRow> kept;
    for (const OutputRow& draft : targetDrafts_) {
        if (draft.kind_index == static_cast<int>(output::OutputTarget::Kind::Link)) {
            kept.push_back(draft);
        }
    }
    OutputRow row{};
    row.address = shared(text);
    row.enabled = true;
    targetDrafts_ = std::move(kept);
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
    draftsPending_ = false;
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
            // Takt4's own messages: the row's, or "global" in a pasted line. The box never holds
            // the word — `formatOutputAddress` does not write it — so a row that was typed keeps
            // what the heading's list gave it.
            if (target.kind == output::OutputTarget::Kind::Osc) {
                target.sendsNamespace = target.sendsNamespace || draft.sends_namespace;
            }
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
            rows.push_back(rowOf(target, deviceNames_));
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

    // **Exactly one Link output, first**, whatever the rows said — a pasted rig with none, or
    // with a second. One that was missing comes back switched as Link is now.
    const bool linkBefore = runner_.snapshot().link;
    if (output::ensureLinkOutput(targets, linkBefore)) {
        std::vector<OutputRow> ordered;
        for (const output::OutputTarget& target : targets) {
            ordered.push_back(rowOf(target, deviceNames_));
        }
        // The unfinished rows, which are not targets yet, after them as they were.
        for (const OutputRow& row : rows) {
            if (std::string(row.address).empty() ||
                !output::findTarget(targets, std::string(row.id))) {
                if (row.kind_index != static_cast<int>(output::OutputTarget::Kind::Link)) {
                    ordered.push_back(row);
                }
            }
        }
        rows = std::move(ordered);
    }
    // A device a row names that the list does not have yet — typed, pasted — goes on it as not
    // plugged in, and the rows find their places in the new list (L32).
    const bool listed = listDevicesFor(targets);
    if (listed) {
        for (OutputRow& row : rows) {
            if (row.kind_index == static_cast<int>(output::OutputTarget::Kind::Midi) ||
                row.kind_index == static_cast<int>(output::OutputTarget::Kind::MidiClock)) {
                splitAddress(row, deviceNames_);
            }
        }
    }
    targetDrafts_ = std::move(rows);
    if (listed) {
        publishPortLists();
    }
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
    // **And never a row built again for what it shows.** Every control on a row follows its row
    // whatever happens to the rows — none of them sets its own value (weltformat.slint) — so a
    // row rewritten in place is a row that shows its new data, and a row built again would only
    // take away the box the operator is typing in or the list they have open: an element somebody
    // is holding is never destroyed.
    //
    // Each row's reason for reaching nothing goes in with it — `rowProblems_`, by id, since the
    // drafts are rebuilt from the targets by every apply: a line of text under the row.
    int on = 0;
    int failing = 0;
    for (OutputRow& row : targetDrafts_) {
        const auto found = rowProblems_.find(std::string(row.id));
        row.problem = shared(found == rowProblems_.end() ? std::string{} : found->second);
        on += row.enabled ? 1 : 0;
        failing += row.problem.empty() ? 0 : 1;
    }
    writeRows(*targetModel_, targetDrafts_);
    // For the line the Outputs heading reads while the section is folded, which a fold must not
    // leave saying all is well while a row below it is reaching nothing.
    window_->set_outputs_on(on);
    window_->set_outputs_failing(failing);
    // And what its "/takt4 global messages to" box says.
    publishGlobalMessages(*window_, targetDrafts_);
}

void WindowController::pickMidiControlPort(int index) {
    const auto port = static_cast<std::size_t>(index);
    const std::vector<std::string> choices = midiInputChoices();
    setMidiControlPort(index <= 0 || port > choices.size() ? std::string{} : choices[port - 1]);
}

void WindowController::setMidiControlPort(const std::string& name) {
    // The bindings survive this: `setPort` keeps them, because an operator moving from
    // one controller to another is not asking to forget what they learned.
    control_.setPort(name);
    midiControlProblem_.clear();
    midiControlRetryAt_ = -1.0;
    // A port asked for that is not on the machine is one of the picker's choices from here.
    publishMidiInputList();
    if (!name.empty()) {
        try {
            control_.start();
            setStatus("Control input on " + control_.portName() +
                          ". Pick an action, press LEARN, then press the control.",
                      false);
        } catch (const std::exception& e) {
            // The port list is what the machine offered when the window opened; a
            // controller unplugged since then lands here, and saying so is the whole
            // reason `start()` throws rather than quietly listening to nothing. And it goes on
            // being said on the input's own line, and tried again every few seconds, so a
            // controller plugged back in — or let go of by the program that had it — is picked
            // up without anybody having to find this dropdown again.
            midiControlProblem_ = midiControlProblem(name);
            midiControlRetryAt_ = nowSeconds() + controlRetrySeconds_;
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

/// The machine's own viewer for a text file. **Never in a test process**: on 2026-10-08 a test
/// whose click sweep crossed the About box's two OPEN buttons, with no file opener of its own,
/// opened the licence in the rig's text editor once per click — 140 Notepad++ windows, until the
/// desktop ran out of window resources and froze. A test that wants to see the opening sets its
/// own opener (`setFileOpener`); one that forgets it now opens nothing.
bool openInViewer(const std::filesystem::path& path) {
    if (sandbox::active()) {
        return false;
    }
#if defined(_WIN32)
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32; // ShellExecute's own convention: anything above 32 is success
#else
    (void)path;
    return false; // said on screen as "written to", which is still somewhere to look
#endif
}

/// Where takt4's source is, as the About box links to it.
constexpr const char* kSourceAddress = "https://github.com/Fohdeesha/takt4";

/// The machine's browser at `address`. Never in a test process: a click sweep over the About box
/// would otherwise open one on the rig's desktop (see `sandbox::active`).
bool openInBrowser(const std::string& address) {
    if (sandbox::active()) {
        return false;
    }
#if defined(_WIN32)
    const std::wstring wide(address.begin(), address.end()); // an address is ASCII
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    (void)address;
    return false;
#endif
}

} // namespace

void WindowController::openAbout() {
    if (!about_) {
        about_ = AboutWindow::create();
        AboutWindow& about = **about_;
        about.set_version(slint::SharedString(versionLabel(buildInfo())));
        about.on_source_opened([this] {
            const bool opened =
                openLink_ ? openLink_(kSourceAddress) : openInBrowser(kSourceAddress);
            // Said where the click was, as the licence buttons say where their file went.
            (*about_)->set_opened(slint::SharedString(
                opened ? std::string()
                       : std::string("No browser opened. The source is at ") + kSourceAddress));
        });
        about.on_licence_opened(
            [this] { (void)openEmbeddedText("takt4-LICENSE.txt", textOf(assets::licence())); });
        about.on_notices_opened([this] {
            (void)openEmbeddedText("takt4-THIRD-PARTY-NOTICES.txt", textOf(assets::notices()));
        });
        about.on_closed([this] { (*about_)->hide(); });
        // The size it was drawn for, before the first show — Slint opens a window at its
        // content's minimum otherwise.
        const LogicalExtent opening = fitToScreen({kAboutWindowWidth, kAboutWindowHeight});
        about.window().set_size(slint::LogicalSize({opening.width, opening.height}));
    }
    (*about_)->show();
    // Forward, if it was already open behind this one — see `RulesController::show`.
    (void)bringWindowToFront("About takt4");
}

std::filesystem::path WindowController::openEmbeddedText(const std::string& name,
                                                         std::string_view text) {
    std::error_code code;
    // `%TEMP%\takt4`, or a test process's own folder — see `settings::scratchDirectory`.
    const std::filesystem::path folder = settings::scratchDirectory();
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
    rulesActive_ = activeRules(rules_);
    // The editor's copy too, or it would go on showing the set it was built with — and the
    // next edit there would post that stale set back over this one.
    editor_.setRules(rules_);
    // The whole set, every time. A rule is small and the set is short, so there is no
    // reason for a finer command — and replacing wholesale is what a preset load does, so
    // the editor and the loader take one road rather than two. **Fresh**, because this is a
    // load — the launch and IMPORT — and not an edit: nothing of the set running before carries
    // over, however its ids match (the audit of 2026-09-25, M11).
    runner_.post(output::OutputCommand::rules(rules_, /*fresh=*/true));

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
    if (!on) {
        // Nothing wanted, so nothing is failing and nothing is to be tried again — whether or
        // not a socket was open. Unticking a port that would not bind has to take its red line
        // away with it.
        oscControlProblem_.clear();
        oscControlRetryAt_ = -1.0;
    }
    if (on == oscControl_.running()) {
        publishControl();
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
        oscControlProblem_.clear();
        oscControlRetryAt_ = -1.0;
        setStatus("OSC control listening on " + std::to_string(oscControl_.port()) +
                      (config.localOnly ? " (this machine only)." : " (any address)."),
                  false);
    } catch (const std::exception& e) {
        // A port another application already has. Saying so is the point: a control
        // surface that silently does nothing is worse than one that will not start. **And
        // going on saying so**, on the line itself: the status line said it once and the next
        // message wrote it away, and the tick box went back to empty as if it had never been
        // asked for — while the saved settings still asked for it. It is tried again every few
        // seconds, so a port the other program lets go of is picked up without a hand on it.
        config.enabled = false;
        oscControl_.setConfig(config);
        oscControlProblem_ = oscControlProblem(e);
        oscControlRetryAt_ = nowSeconds() + controlRetrySeconds_;
        setStatus(e.what(), true);
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
    // a ComboBox cannot be moved from outside by its value at all (Slint 11970). Found by the
    // rule the port is opened by, among the choices — a port not plugged in is one of them.
    const std::string& wanted = control_.config().port;
    const std::optional<std::size_t> at = output::findMidiPort(midiInputChoices(), wanted);
    window_->set_midi_in_port_index(wanted.empty() || !at ? 0 : static_cast<int>(*at) + 1);

    if (!control_.running()) {
        // A port asked for and not open is a fault, and says which and why for as long as it
        // lasts; the status line said it once.
        const bool failing = !control_.config().port.empty() && !midiControlProblem_.empty();
        window_->set_control_error(failing);
        window_->set_control_reading_words(true);
        window_->set_control_reading(shared(
            failing ? "NOT OPEN \xE2\x80\x94 " + midiControlProblem_ + ". Trying again every " +
                          fixed(controlRetrySeconds_, 0) + " s."
            : midiInputPorts_.empty() ? std::string("no MIDI inputs on this machine")
                                      : std::string("off \xE2\x80\x94 pick a port")));
        return;
    }
    window_->set_control_error(false);
    if (control_.learning()) {
        window_->set_control_reading_words(true);
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
    // A binding is a control, in DM Mono; "not bound" is a state, in words.
    window_->set_control_reading_words(text.empty());
    if (text.empty()) {
        // Nothing bound. Say what did arrive instead, if anything has: a controller on a
        // channel nothing is listening to looks exactly like a broken cable otherwise.
        const std::optional<control::MidiEvent> last = control_.lastEvent();
        text = last ? "not bound - last seen " + describeControl(*last) : std::string("not bound");
    }
    window_->set_control_reading(shared(text));
}

void WindowController::publishTriggers() {
    // Counted when the rules change, not here: this runs thirty times a second, and building
    // a whole `Rule` per rule to ask whether it is valid was that many times the work for a
    // number that moves only when somebody edits (the audit's Low items).
    window_->set_rules_active(rulesActive_);
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
    // **What was asked for**, not what bound: a port another program holds leaves the tick as
    // it was put, beside a line that says in red that it is not listening and why. The tick
    // going back to empty read as "never asked for" — while the file saved it as asked for.
    window_->set_osc_control_on(listening || oscControlWanted_);
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
        const bool failing = oscControlWanted_ && !oscControlProblem_.empty();
        window_->set_osc_control_error(failing);
        window_->set_osc_control_reading_words(true);
        window_->set_osc_control_reading(shared(
            failing ? "NOT LISTENING \xE2\x80\x94 " + oscControlProblem_ + ". Trying again every " +
                          fixed(controlRetrySeconds_, 0) + " s."
                    : std::string("off")));
        window_->set_osc_control_from(shared(""));
        window_->set_osc_control_counts(shared(""));
        return;
    }
    window_->set_osc_control_error(false);
    window_->set_osc_control_reading_words(false);
    // Two different questions, and only one of them is live at a time. Before anything has
    // arrived the operator needs the address to aim at; once packets are landing they need
    // to know what landed, and the address has answered itself.
    const std::uint64_t handled = oscControl_.handled();
    const std::uint64_t ignored = oscControl_.ignored();
    if (handled == 0 && ignored == 0) {
        window_->set_osc_control_reading(shared(config.prefix + "/ctl/...  nothing yet"));
        window_->set_osc_control_from(shared(""));
        window_->set_osc_control_counts(shared(""));
        return;
    }
    // The address on the first line and where it came from beside it, in the secondary colour;
    // the counts under them (the approved render, HANDOFF §0.5).
    std::string address = oscControl_.lastMessage();
    std::string from;
    if (const std::size_t at = address.rfind("  from "); at != std::string::npos) {
        from = address.substr(at + 2);
        address.erase(at);
    }
    window_->set_osc_control_reading(shared(address));
    window_->set_osc_control_from(shared(from));
    // Counted separately, because "arriving but not understood" is a different fault from
    // "not arriving" and they need different fixes — a wrong prefix against a wrong
    // address. Nothing else on screen tells the two apart.
    window_->set_osc_control_counts(
        shared(std::to_string(handled) + " acted, " + std::to_string(ignored) + " ignored"));
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

void WindowController::setLatencyTyped(const std::string& text) {
    const std::optional<double> ms = readMilliseconds(text);
    if (!ms) {
        setStatus("\"" + text + "\" is not a number of milliseconds, so the latency was left at " +
                      std::to_string(
                          static_cast<int>(std::lround(settings().latencyOffsetSeconds * 1000.0))) +
                      " ms.",
                  true);
        return;
    }
    // `postOptions` writes the clamped value back into the window, which moves the slider.
    setLatencyMs(*ms);
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
    if (outage_ && input_) {
        // The input that went quiet is the one wanted, whatever the picker shows while it is
        // being looked for — autosave runs through an outage, and it used to save whatever sat
        // at the old position of a list the outage had read again (the audit of 2026-09-25, H1).
        out.machine.deviceName = input_->device.name;
        out.machine.hostApiName = input_->device.hostApiName;
        out.machine.channel = input_->selection.channels.front();
        out.machine.mono = mono_;
    } else if (deviceFallback_ && !deviceChosen_) {
        // The remembered interface was not here and nobody picked another — so it is still the
        // one wanted. Saving the fallback as if chosen made one launch before the MOTU was
        // powered on move the next show to the wrong device at channel 1 (the audit's H11).
        out.machine.deviceName = remembered_.deviceName;
        out.machine.hostApiName = remembered_.hostApiName;
        out.machine.channel = remembered_.channel;
        out.machine.mono = remembered_.mono;
    } else if (device_ >= 0 && static_cast<std::size_t>(device_) < devices_.size()) {
        const audio::InputDevice& device = devices_[static_cast<std::size_t>(device_)];
        out.machine.deviceName = device.name;
        out.machine.hostApiName = device.hostApiName;
        out.machine.channel = channel_;
        out.machine.mono = mono_;
    } else {
        out.machine.mono = mono_;
    }
    // **The snapshot, not the live transports.** SAVE and EXPORT are pressed while a set is
    // running, and `OutputRunner::transports()` hands back references the output thread
    // replaces whole — a vector of targets, an optional port. Copying one while that thread
    // reassigns it is a freed buffer, not a stale reading. `publishOutputs` already reads
    // this way; this was the one place left that did not.
    const output::OutputRunner::Snapshot live = runner_.snapshot();
    // The MIDI clocks are outputs now, each keeping the device it asked for whether or not it
    // opened — what a port unplugged at this launch needs to still be wanted at the next (the
    // audit's H11). `settings::toJson` writes the first one where an older build looks too.

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
    out.machine.inputsFolded = window_->get_inputs_folded();
    out.machine.outputsFolded = window_->get_outputs_folded();
    editor_.layoutInto(out.machine);
    patch_.layoutInto(out.machine);

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
    out.preset.library = library_;
    return out;
}

bool WindowController::saveNow() {
    // An output row still being typed in is what the operator means to save: SAVE reads the
    // runner's list, and the row's own commit comes a turn of the event loop after this click
    // (the audit of 2026-09-25, M15).
    applyDrafts();
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
    applyDrafts(); // see `saveNow`
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
    if (text.empty()) {
        return;
    }
    // In front of what the window met on the way up rather than over it: the rig file's own
    // state is the first thing to read, and the rest still has to be read (M13).
    holdStartupMessage(startupMessage_.empty() ? text : text + "  ·  " + startupMessage_);
}

void WindowController::holdStartupMessage(const std::string& text) {
    startupMessage_ = text;
    startupHeldUntil_ = nowSeconds() + kStartupHoldSeconds;
    showStatus(text, true);
}

void WindowController::report(const std::string& text, bool error) {
    // **What the window finds out on its own, in its first seconds, joins what it met starting
    // rather than replacing it** (the audit of 2026-09-25, M13). The first redraw said which
    // outputs could not be reached — over a damaged settings file's notice, before anybody could
    // have read that. Anything the operator does says what it did as usual (`setStatus`), and
    // after the hold everything does.
    if (!startupMessage_.empty() && nowSeconds() < startupHeldUntil_) {
        if (startupMessage_.find(text) == std::string::npos) {
            startupMessage_ += "  ·  " + text;
        }
        showStatus(startupMessage_, true);
        return;
    }
    setStatus(text, error);
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
    // Finished before anything is read or replaced, as a click anywhere else would finish it —
    // see `saveNow`.
    applyDrafts();
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
    // rules — so the file is checked for being *settings* before any of it is applied. And
    // **said why, when it is not** (the audit of 2026-09-25, L29): through `load`, a file
    // damaged by a hand edit or held by another program was "no preset in it", which sends the
    // operator looking for the wrong thing.
    const settings::Loaded checked = settings::loadChecked(path);
    if (checked.status == settings::LoadStatus::Corrupt ||
        checked.status == settings::LoadStatus::Unreadable) {
        setStatus("Cannot import " + io::pathText(path.filename()) + ": " +
                      (checked.status == settings::LoadStatus::Corrupt ? "it is not a settings "
                                                                         "file takt4 can read ("
                                                                       : "it could not be opened (") +
                      checked.problem + "). Nothing was changed.",
                  true);
        return false;
    }
    const settings::Settings& loaded = checked.settings;
    // **The whole preset half against a fresh install's**, not four fields of it: this used to
    // look at the rules, the outputs, the prefix and the meters, so a file holding nothing but
    // a lighting patch — or a tempo window, or a decoder — was "no preset" (the audit's M18).
    // A machine section is something too, since IMPORT restores it (2026-10-03).
    if (!loaded.machine.inFile) {
        settings::Settings imported;
        imported.preset = loaded.preset;
        // Less the Link output every loaded set is given, when it is only that — switched off,
        // no delay — which a file says nothing about, and whose id is new at every load.
        std::vector<output::OutputTarget>& outputs = imported.preset.outputs;
        if (!outputs.empty() && outputs.front().kind == output::OutputTarget::Kind::Link &&
            !outputs.front().enabled && outputs.front().delaySeconds == 0.0) {
            outputs.erase(outputs.begin());
        }
        if (settings::toJson(imported) == settings::toJson(settings::Settings{})) {
            setStatus(io::pathText(path.filename()) +
                          " has no settings in it, so nothing was changed.",
                      true);
            return false;
        }
    }

    // The preset half first. The outputs are the preset's, and Link and the MIDI clocks are
    // outputs since 2026-09-25, so they come with them. The machine half last — see the end.
    setRules(loaded.preset.rules);
    meters_ = loaded.preset.meters;
    postOptions(loaded.preset.tempo);
    // **The lighting patch too** (M18): an import used to leave the patch alone, so every
    // imported lighting rule aimed at fixtures this rig did not have, and reached nothing.
    fixtures_ = loaded.preset.fixtures;
    // And the definitions its fixtures were imported from, which travel with them.
    library_ = loaded.preset.library;
    runner_.post(output::OutputCommand::patch(fixtures_));
    patch_.setFixtures(fixtures_, library_);
    editor_.setPatch(fixtures_);
    // The decoder and the OSC prefix are fixed for as long as the application runs — the
    // engine is built with one, and a receiver is configured for the other — so a preset that
    // carries different ones is kept for the next launch rather than dropped, and says so.
    //
    // **This import's, not the one before's** (the audit of 2026-09-25, L30): a second import
    // with the running decoder and prefix left the first import's still pending, so the next
    // launch came up with a decoder and a prefix from a file the operator had since replaced.
    std::string restart;
    pendingDecoder_.reset();
    pendingPrefix_.reset();
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
    // **And the meters**, which the tracker is built with as it is with the decoder: kept for the
    // next launch, and — like the decoder — said. They were kept and not said, so the bars went
    // on being counted in the old meters with nothing to say why.
    if (meters_ != tracker_.engine().meters()) {
        restart += restart.empty() ? "the meters" : " and the meters";
    }

    // The rows the operator edits, not just the transports: these are the window's copy from
    // construction onwards (see the constructor), so an import that changed only the
    // transports would leave the boxes showing the old rig.
    targetDrafts_.clear();
    ++outputsGeneration_; // another rig under the boxes: see `outputsGeneration_`
    const bool listed = listDevicesFor(loaded.preset.outputs);
    for (const output::OutputTarget& target : loaded.preset.outputs) {
        targetDrafts_.push_back(rowOf(target, deviceNames_));
    }
    publishTargetRows();
    if (listed) {
        publishPortLists();
    }
    applyTargets();

    // **And this machine's half, when the file has one** (the operator, 2026-10-03: an import
    // "should carry/restore EVERYTHING"). It used to be left alone by design — Q7 kept the input
    // and the learned controls out of anything that travels — so a rig restored from its own
    // export came back without its input, its pads, its OSC control or its layout.
    const std::string input = loaded.machine.inFile ? applyMachine(loaded.machine) : std::string{};

    // **What it did to the input first**, when it did anything: the status shows two lines and
    // keeps the first, and an import that took the show off its input is the part that must not
    // be cut off the end. It used to say only how many rules it brought.
    const std::string summary =
        (input.empty() ? std::string{} : input + " ") + "Imported " +
        io::pathText(path.filename()) + ": " + std::to_string(loaded.preset.rules.size()) +
        " rules, " + std::to_string(loaded.preset.outputs.size()) + " outputs, " +
        std::to_string(loaded.preset.fixtures.size()) + " fixtures" +
        (loaded.machine.inFile ? ", the input, MIDI and OSC control and the layout." : ".") +
        (restart.empty() ? std::string{} : " Restart takt4 for " + restart + " to take effect.");
    // The START it asked for says this again once the input is open, in place of its own line
    // (`toggleRun`); an import that stopped listening says it as the trouble it is.
    const bool restarting = !input.empty() && runPending_;
    setStatus(summary, !input.empty() && !restarting);
    importSaid_ = restarting ? summary : std::string{};
    return true;
}

void WindowController::publishOutputs() {
    // **A snapshot, not the live transports.** This runs on the redraw timer and the output
    // thread owns what it is reading: `outputs()` is a vector of strings that thread replaces
    // whole whenever a target changes, and `applyTargets` posts one of those and then calls
    // this — so on a running rig the two overlap by design, sixty times through a delay-slider
    // drag. Copying a vector while another thread reassigns it is a freed buffer, not a stale
    // number. `OutputRunner::snapshot` is taken under a lock and is at worst a millisecond old.
    //
    // And copied only when a new one has been taken: this runs thirty times a second, the copy
    // is every target and every fixture, and between edits it is the same one every time.
    const std::uint64_t version = runner_.snapshotVersion();
    const bool fresh = version != snapshotShown_;
    if (fresh) {
        snapshot_ = runner_.snapshot();
        snapshotShown_ = version;
    }
    const output::OutputRunner::Snapshot& live = snapshot_;
    // Link's own, and safe to ask from any thread — as is the beat count, which is atomic.
    window_->set_link_peers(static_cast<int>(runner_.transports().link().numPeers()));
    // The list of them closes with Link: switched off, there is no session to list.
    if (linkPeersShown_ && !live.link) {
        toggleLinkPeers();
    }
    publishLinkPeers();

    // The rows themselves are **not** written here. They are what is being typed into, this
    // runs on the redraw timer, and a row replaced under the cursor is a row that cannot be
    // edited while the tracker runs. `applyTargets` owns them; see `targetDrafts_`.
    // The editor names these when it routes a rule, so it has to know what there is. Told
    // here rather than read from the runner, because this is the one place already holding a
    // safe copy — and only when that copy is new.
    if (fresh) {
        editor_.setTargets(live.outputs);
    }

    window_->set_beats_sent(static_cast<int>(runner_.transports().beats()));
    publishLostMidi(live.lostMidi, live.outputs);
    publishOutputProblems(live.outputProblems);
    publishRowProblems(live);
    publishOutputTrouble(live.trouble);
}

void WindowController::publishOutputTrouble(const output::OutputRunner::Snapshot::Trouble& trouble) {
    if (trouble == outputTroubleShown_) {
        return;
    }
    // A stage that threw with something new to say is said on the status line too. Not on every
    // count: a stage throwing every round would otherwise write the line a thousand times a
    // second and nothing else could ever be read there.
    if (trouble.roundErrors > outputTroubleShown_.roundErrors &&
        trouble.lastRoundError != outputTroubleShown_.lastRoundError) {
        setStatus("The output thread met an error in " + trouble.lastRoundError +
                      ". The rest of it carried on.",
                  true);
    }
    outputTroubleShown_ = trouble;
    std::vector<std::string> parts;
    if (trouble.roundErrors != 0) {
        parts.push_back(counted(trouble.roundErrors, "output error", "output errors",
                                "the last in " + trouble.lastRoundError));
    }
    if (trouble.undeliverable != 0) {
        parts.push_back(counted(trouble.undeliverable, "rule message", "rule messages",
                                "reached no output"));
    }
    if (trouble.heldDropped != 0) {
        parts.push_back(counted(trouble.heldDropped, "delayed message", "delayed messages",
                                "dropped, too many were waiting"));
    }
    if (trouble.clockTicksSkipped != 0) {
        parts.push_back(counted(trouble.clockTicksSkipped, "MIDI clock tick", "MIDI clock ticks",
                                "skipped after a stall"));
    }
    window_->set_output_trouble(shared(joined(parts)));
}

void WindowController::retryControls(double now) {
    if (oscControlWanted_ && !oscControl_.running() && oscControlRetryAt_ >= 0.0 &&
        now >= oscControlRetryAt_) {
        oscControlRetryAt_ = now + controlRetrySeconds_;
        control::OscControl::Config config = oscControl_.config();
        config.enabled = true;
        oscControl_.setConfig(config);
        try {
            oscControl_.start();
            oscControlProblem_.clear();
            oscControlRetryAt_ = -1.0;
            report("OSC control listening on " + std::to_string(oscControl_.port()) +
                       " \xE2\x80\x94 the port is free again.",
                   false);
        } catch (const std::exception& e) {
            // Still taken. Said on its own line already; the status line is not written again
            // every few seconds with the same thing.
            config.enabled = false;
            oscControl_.setConfig(config);
            oscControlProblem_ = oscControlProblem(e);
        }
        publishOscControl(false);
    }
    const std::string& port = control_.config().port;
    if (!port.empty() && !control_.running() && midiControlRetryAt_ >= 0.0 &&
        now >= midiControlRetryAt_) {
        midiControlRetryAt_ = now + controlRetrySeconds_;
        try {
            control_.start();
            midiControlProblem_.clear();
            midiControlRetryAt_ = -1.0;
            report("Control input on " + control_.portName() + " \xE2\x80\x94 it is back.", false);
        } catch (const std::exception&) {
            midiControlProblem_ = midiControlProblem(port);
        }
        publishMidiControl();
    }
}

void WindowController::publishRowProblems(const output::OutputRunner::Snapshot& live) {
    std::map<std::string, std::string> problems;
    for (const output::Transports::Problem& problem : live.outputProblems) {
        problems[problem.id] = problem.why;
    }
    for (const output::OutputTarget& target : live.outputs) {
        const bool device = target.kind == output::OutputTarget::Kind::Midi ||
                            target.kind == output::OutputTarget::Kind::MidiClock;
        if (device && target.enabled && problems.count(target.id) == 0 &&
            std::find(live.lostMidi.begin(), live.lostMidi.end(), target.device) !=
                live.lostMidi.end()) {
            problems[target.id] = "\"" + target.device +
                                  "\" has stopped responding \xE2\x80\x94 unplugged? takt4 keeps "
                                  "trying to reopen it";
        }
        if (target.kind == output::OutputTarget::Kind::Link && !linkPeersProblem_.empty()) {
            problems[target.id] = linkPeersProblem_;
        }
    }
    if (problems == rowProblems_) {
        return;
    }
    rowProblems_ = std::move(problems);
    publishTargetRows();
}

void WindowController::publishOutputProblems(
    const std::vector<output::Transports::Problem>& found) {
    std::vector<std::string> problems;
    for (const output::Transports::Problem& problem : found) {
        problems.push_back(problem.text);
    }
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
        report("outputs: " + text + ".", true);
    } else if (problems.empty() && !outputProblemsShown_.empty()) {
        report("outputs: every output can be reached again.", false);
    }
    outputProblemsShown_ = problems;
}

void WindowController::publishLostMidi(const std::vector<std::string>& lost,
                                       const std::vector<output::OutputTarget>& outputs) {
    if (lost == lostMidiShown_) {
        return;
    }
    // Said when it changes, in words: a lighting desk or a sequencer that has stopped hearing
    // takt4 is otherwise invisible from here, and the output thread is already trying to bring
    // it back once a second.
    for (const std::string& name : lost) {
        if (std::find(lostMidiShown_.begin(), lostMidiShown_.end(), name) == lostMidiShown_.end()) {
            report("MIDI device \"" + name +
                       "\" has stopped responding — unplugged? takt4 keeps trying to reopen it.",
                   true);
        }
    }
    for (const std::string& name : lostMidiShown_) {
        // Back only if something still sends to it: a device leaves this list as well when its
        // row is removed or switched off, and "is back" would be untrue of one still unplugged.
        const bool used = std::any_of(outputs.begin(), outputs.end(), [&](const auto& target) {
            return target.enabled &&
                   (target.kind == output::OutputTarget::Kind::Midi ||
                    target.kind == output::OutputTarget::Kind::MidiClock) &&
                   target.device == name;
        });
        if (used && std::find(lost.begin(), lost.end(), name) == lost.end()) {
            report("MIDI device \"" + name + "\" is back.", false);
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

std::string WindowController::latencyLine() const {
    const audio::InputStream* stream = tracker_.stream();
    if (stream == nullptr) {
        return {};
    }
    const double resamplerMs =
        1000.0 * static_cast<double>(stream->resamplerDelayFrames()) / stream->sampleRate();
    // **Taken out of every beat's time**: each beat is stamped with when its audio was at the
    // input, whatever the buffer size, so none of these moves an output — and a buffer size
    // changed in the driver's panel reopens the input and is said again here. The figures alone:
    // "latency taken out of the beat times: " before them made the line so long the window had
    // to be widened a long way to stop it wrapping (the operator, 2026-10-08).
    return fixed(stream->inputLatencySeconds() * 1000.0, 1) + " ms input + " +
           fixed(resamplerMs, 1) + " ms resampler + 40.0 ms centred framing";
}

void WindowController::publishOpenStream(const std::string& said) {
    const audio::InputStream* stream = tracker_.stream();
    if (stream == nullptr || !tracker_.current()) {
        return;
    }
    // An import that moved the show to this input said so, and that is what stays said: see
    // `importFrom`. It fills the two lines itself, so the latency figures wait for the next START.
    if (!said.empty()) {
        setStatus(said, false);
        return;
    }
    const engine::LiveTracker::Running& running = *tracker_.current();
    const std::string inputs =
        running.selection.count == 2
            ? "In " + std::to_string(running.selection.channels[0] + 1) + " + " +
                  std::to_string(running.selection.channels[1] + 1) + " (stereo)"
            : inputName(running.selection.channels[0]);
    // Two lines: what is being listened to, and the latency figures on a line of their own
    // (asked for on 2026-09-29, with the redesign), so neither is broken across the other.
    setStatus(inputs + " of " + running.device.name + " \xC2\xB7 " +
                  fixed(stream->sampleRate(), 0) + " Hz \xE2\x86\x92 " +
                  fixed(audio::kInternalSampleRate, 0) + " Hz \xC2\xB7 " +
                  audio::toString(stream->picker().mode()) + " pick\n" + latencyLine(),
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
    // Whatever the window says now is news the startup message gives way to (M13; see `report`).
    startupMessage_.clear();
    showStatus(text, error);
}

void WindowController::showStatus(const std::string& text, bool error) {
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

void WindowController::superviseNow(double now) {
    // Once at a time: a dialog opened from inside a round would otherwise run the supervision a
    // second time, inside the first.
    if (!wantRunning_ || supervising_) {
        return;
    }
    supervising_ = true;
    struct Done {
        bool& flag;
        ~Done() { flag = false; }
    } const done{supervising_};
    audio::InputWatchdog::Reading reading;
    audio::AsioDriverEvents events;
    if (!outage_ && tracker_.stream() != nullptr) {
        if (input_ && input_->selection.count == 2) {
            superviseStereo(tracker_.stream()->stereo(), now);
        }
        reading = watchdog_.observe(tracker_.stream()->counters(), now);
        if (input_ && input_->device.hostApi == audio::HostApiKind::Asio) {
            events = audio::takeAsioDriverEvents();
        }
    }
    superviseInput(reading, events, now);
}

void WindowController::superviseThroughModalLoops() {
    if (modalPump_ == 0) {
        modalPump_ = pumpThroughModalLoops(&WindowController::superviseWhileBlocked, this,
                                           kModalPumpMilliseconds);
    }
}

void WindowController::superviseWhileBlocked(void* self) {
    // From Windows' own timer, which every message loop on this thread serves — a file dialog's,
    // the system menu's, a message box's — where Slint's timers, and so `tick`, do not run. Only
    // when `tick` has not: it supervises the input itself.
    auto* const controller = static_cast<WindowController*>(self);
    const double now = controller->nowSeconds();
    if (now - controller->lastTickAt_ < kTickMissedSeconds) {
        return;
    }
    controller->superviseNow(now);
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
    lastTickAt_ = nowSeconds();
    // START or STOP carried out a moment ago: the button takes presses again. See
    // `requestToggleRun`.
    if (!runPending_ && window_->get_run_busy() &&
        nowSeconds() - runSettledAt_ >= kRunGraceSeconds) {
        window_->set_run_busy(false);
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
    // A control input that was asked for and would not open, tried again when it is due — so a
    // port another program lets go of, or a controller plugged back in, is picked up by itself.
    retryControls(nowSeconds());
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
    superviseNow(nowSeconds());
    publishDriverState();
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
    const tracking::TempoState state = tracker_.engine().state();
    publishTempoState(*window_, state);
    publishBeatDots(state);
    // After it: publishTempoState shows what the engine has, and this is the one property
    // the window may legitimately be showing ahead of it.
    publishPin();
    publishOptions();
}

void WindowController::publishBeatDots(const tracking::TempoState& state) {
    if (state.beatInBar == 0) {
        // Stopped, reset, or no beat yet: nothing fired before this holds.
        dotBeatInBar_ = 0;
        dotTimer_.stop();
        return;
    }
    const output::OutputRunner::ShownBeat shown =
        shownBeatSource_ ? shownBeatSource_() : runner_.shownBeat();
    if (shown.serial != dotSerial_) {
        dotSerial_ = shown.serial;
        const auto show = [this](std::uint32_t beatInBar, std::uint64_t bars) {
            dotBeatInBar_ = beatInBar;
            dotBars_ = bars;
            window_->set_beat_in_bar(static_cast<int>(beatInBar));
            window_->set_bars(static_cast<int>(bars));
        };
        // Ahead of its time, as a locked beat is whenever the latency leaves room: lit when it is
        // played, by a timer of its own rather than on whichever redraw comes after.
        const double wait = shown.due - runner_.elapsed();
        if (wait > 0.001) {
            dotTimer_.start(slint::TimerMode::SingleShot,
                            std::chrono::milliseconds(std::llround(wait * 1000.0)),
                            [show, shown] { show(shown.beatInBar, shown.bars); });
        } else {
            dotTimer_.stop();
            show(shown.beatInBar, shown.bars);
        }
    }
    if (dotBeatInBar_ != 0) {
        window_->set_beat_in_bar(static_cast<int>(dotBeatInBar_));
        window_->set_bars(static_cast<int>(dotBars_));
    }
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
