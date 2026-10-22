#include "ui/window_state.hpp"

#include "core/audio/hop_meter.hpp"
#include "core/io/chars.hpp"
#include "core/output/midi_ports.hpp"
#include "core/output/output_target.hpp"
#include "core/settings/settings.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace takt4::ui {

std::optional<double> readMilliseconds(std::string_view text) {
    const auto trim = [](std::string_view view) {
        while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
            view.remove_prefix(1);
        }
        while (!view.empty() && (view.back() == ' ' || view.back() == '\t')) {
            view.remove_suffix(1);
        }
        return view;
    };
    text = trim(text);
    if (text.size() >= 2 &&
        (text.ends_with("ms") || text.ends_with("MS") || text.ends_with("Ms"))) {
        text = trim(text.substr(0, text.size() - 2));
    }
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1); // `from_chars` takes a '-' and not a '+'
    }
    if (text.empty()) {
        return std::nullopt;
    }
    double value = 0.0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result = io::fromChars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::string fixed(double value, int places) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(places) << value;
    return out.str();
}

std::string shortVersionLabel(std::string_view version, std::string_view commit) {
    // `versionLabel`'s own test for a release: the commit is the release's tag.
    const bool release =
        commit.size() == version.size() + 1 && commit.front() == 'v' && commit.substr(1) == version;
    return std::string(version) + (release ? "\nrelease" : "\ndev");
}

TracePoint tracePoint(const engine::EngineFrame& frame) {
    // By name rather than by position: the generated struct happens to keep the order the
    // markup declares, but nothing promises that across a Slint bump.
    TracePoint point;
    point.beat = frame.activation.beat;
    point.downbeat = frame.activation.downbeat;
    point.called = frame.beat;
    return point;
}

void publishTempoState(MainWindow& window, const tracking::TempoState& state) {
    window.set_bpm(static_cast<float>(state.bpm));
    window.set_raw_bpm(static_cast<float>(state.rawBpm));
    window.set_called_bpm(static_cast<float>(state.calledBpm));
    window.set_locked(state.locked);
    window.set_pinned(state.pinned);
    window.set_holding(state.holding);
    window.set_no_signal(state.noSignal);
    window.set_acquired(state.acquired);
    window.set_refined(state.refined);
    window.set_confidence(static_cast<float>(state.confidence));
    window.set_beats_per_bar(static_cast<int>(state.beatsPerBar));
    window.set_beat_in_bar(static_cast<int>(state.beatInBar));
    window.set_bars(static_cast<int>(state.bars));
    window.set_beat_divisor(static_cast<int>(state.beatDivisor));
    window.set_octave_shift(static_cast<int>(state.octaveShift));
    window.set_beats_bpm(static_cast<float>(state.beatsBpm));
}

void publishTempoOptions(MainWindow& window, const tracking::TempoTracker::Options& options,
                         std::optional<double> followedLatency) {
    window.set_fold_on(options.octaveFold);
    window.set_fold_min(static_cast<float>(options.minBpm));
    window.set_fold_max(static_cast<float>(options.maxBpm));
    window.set_latency_ms(
        static_cast<float>(followedLatency.value_or(options.latencyOffsetSeconds) * 1000.0));
    window.set_keep_shift(options.keepOctaveShift);
}

namespace {

/// "3 players, 1 mixer".
std::string devicesHeard(std::size_t players, std::size_t mixers) {
    std::string text = std::to_string(players) + (players == 1 ? " player" : " players");
    if (mixers != 0) {
        text += ", " + std::to_string(mixers) + (mixers == 1 ? " mixer" : " mixers");
    }
    return text;
}

/// See `describeTiming`: the CDJs.
TimingView describeCdjs(const TimingFacts& facts, TimingView view) {
    const std::string dot = " \xC2\xB7 ";
    view.name = "CDJs";
    view.shortName = "CDJ";
    // The CDJs count four to the bar, whatever the setting says.
    view.beatsPerBar = 4;
    view.network =
        facts.cdjInterface.empty() ? std::string("picked when a CDJ is heard") : facts.cdjInterface;
    const bool heard = facts.cdjPlayers + facts.cdjMixers > 0;
    if (!facts.running) {
        // Listened to, and nothing announced, until START.
        if (!facts.cdjProblem.empty()) {
            view.reading = facts.cdjProblem;
            view.waiting = true;
        } else if (!heard) {
            view.reading = "no CDJs heard on this network yet";
        } else {
            view.reading = devicesHeard(facts.cdjPlayers, facts.cdjMixers) + " heard" + dot +
                           "not joined until Start";
        }
        return view;
    }
    view.coasting = facts.coasting;
    view.waiting = !facts.state.locked && !facts.coasting;
    view.stripBeat = static_cast<int>(facts.state.beatInBar);
    const std::string bpm = fixed(facts.state.rawBpm, 2) + " BPM";
    // Which player takt4 is goes with the card it is on: a detail, after what is followed.
    if (facts.cdjJoined && facts.cdjNumber != 0) {
        view.network += dot + "takt4 is player " + std::to_string(facts.cdjNumber);
    }
    const auto playerName = [&](std::uint8_t number) {
        std::string text = "player " + std::to_string(number);
        if (number == facts.cdjMaster) {
            text += " (master)";
        }
        return text;
    };
    const std::string waitedFor =
        facts.cdjFollow == 0 ? std::string("no master playing")
                             : "player " + std::to_string(facts.cdjFollow) + " not playing";
    if (!facts.cdjProblem.empty()) {
        view.waiting = true;
        view.waitingWord = "no network";
        view.reading = facts.cdjProblem;
        view.detail = facts.cdjProblem;
    } else if (!facts.cdjJoined) {
        view.waitingWord = "joining";
        view.reading = heard ? "joining as a player\xE2\x80\xA6"
                             : "no CDJs heard on this network \xE2\x80\x94 waiting for one";
        view.detail = heard ? "joining as a player" : "no CDJs heard";
    } else if (view.coasting) {
        view.reading = "beats stopped \xE2\x80\x94 coasting at " + bpm;
        view.detail = playerName(facts.cdjFollowed) + " stopped";
    } else if (view.waiting) {
        view.waitingWord = facts.cdjFollow == 0 ? "no master" : "no beats";
        view.reading = waitedFor + " \xE2\x80\x94 waiting";
        view.detail = waitedFor;
        if (facts.cdjFollow == 0 && facts.cdjMasterWithoutBeats) {
            // A master with a track rekordbox has not analysed, or a CD: it leads nothing.
            view.note = "the master's track has no beat grid, so it sends no beats";
        }
    } else {
        view.reading = "following " + playerName(facts.cdjFollowed) + dot + bpm;
        view.detail = playerName(facts.cdjFollowed) + dot + "4 beats to the bar";
    }
    if (view.stripBeat > 0 && facts.cdjFollowed != 0) {
        view.stripLabel = "player " + std::to_string(facts.cdjFollowed) + dot +
                          std::to_string(view.stripBeat) + " of 4";
    }
    return view;
}

} // namespace

TimingView describeTiming(const TimingFacts& facts) {
    TimingView view;
    view.index = facts.index;
    view.beatsPerBar = static_cast<int>(facts.beatsPerBar);
    view.obey = facts.obey;
    view.ready = facts.ready;
    if (facts.index == 3) {
        return describeCdjs(facts, view);
    }
    // Running, the box shows the bar the run counts in, and says when the setting is another for
    // the next START (an import while running).
    std::string meterNote;
    if (facts.running && facts.runBeatsPerBar != 0 && facts.runBeatsPerBar != facts.beatsPerBar) {
        view.beatsPerBar = static_cast<int>(facts.runBeatsPerBar);
        meterNote = std::to_string(facts.beatsPerBar) + " beats to the bar from the next START";
    }
    if (facts.index != 1 && facts.index != 2) {
        return view; // beat detection: the tracker's own readouts say it all
    }
    const bool link = facts.index == 1;
    view.name = link ? "Ableton Link" : "MIDI clock";
    view.shortName = link ? "Link" : "MIDI";
    if (!facts.running) {
        // Nothing joined or opened before START; Link's peers are heard all the same.
        if (link) {
            view.reading = !facts.canListen ? "cannot listen for Link sessions on this network"
                           : facts.peersHeard == 0
                               ? "no Link session heard"
                               : std::to_string(facts.peersHeard) +
                                     (facts.peersHeard == 1 ? " peer" : " peers") +
                                     " seen \xC2\xB7 " + fixed(facts.heardBpm, 2) + " BPM";
        }
        return view;
    }
    view.coasting = facts.coasting;
    view.note = meterNote;
    // Waiting for the source: started, and nothing to follow — never heard since START, or gone
    // for longer than the coast.
    view.waiting = !facts.state.locked && !facts.coasting;
    view.waitingWord = link ? "no peers" : "no clock";
    view.stripBeat = static_cast<int>(facts.state.beatInBar);
    if (view.stripBeat > 0) {
        view.stripLabel =
            "beat " + std::to_string(view.stripBeat) + " of " + std::to_string(view.beatsPerBar);
    }
    const std::string bpm = fixed(facts.state.rawBpm, 2) + " BPM";
    if (link) {
        const std::string peers =
            facts.peers == 1 ? std::string("1 peer") : std::to_string(facts.peers) + " peers";
        view.reading = view.waiting ? "no peers \xE2\x80\x94 waiting for a Link session"
                                    : peers + " \xC2\xB7 " + bpm;
        view.detail = view.waiting ? "waiting for a Link session"
                                   : peers + " \xC2\xB7 " + std::to_string(view.beatsPerBar) +
                                         " beats to the bar";
        return view;
    }
    const std::string port = facts.port.empty() ? std::string("no port") : facts.port;
    if (facts.portLost) {
        // Its device pulled out: no clock can come until it is back, and the hub opens it again
        // the moment it is.
        view.reading = "NOT OPEN \xE2\x80\x94 " + port + " is not plugged in";
        view.detail = port + " \xC2\xB7 not plugged in";
        view.waitingWord = "unplugged";
    } else if (facts.stopObeyed) {
        view.reading = "stopped \xE2\x80\x94 waiting for Start or Continue";
        view.detail = port + " \xC2\xB7 stopped";
    } else if (facts.coasting) {
        view.reading = "no clock \xE2\x80\x94 coasting at " + bpm;
        view.detail = port + " \xC2\xB7 clock stopped";
    } else if (view.waiting) {
        view.reading = facts.ticking ? "ticking \xE2\x80\x94 measuring the tempo"
                                     : "no clock \xE2\x80\x94 waiting for one on " + port;
        view.detail = port + (facts.ticking ? " \xC2\xB7 measuring" : " \xC2\xB7 no clock");
    } else {
        view.reading = "ticking \xC2\xB7 " + bpm;
        view.detail = port + (facts.placed ? " \xC2\xB7 started" : " \xC2\xB7 no start yet");
        if (!facts.placed) {
            // A DJ mixer's clock gives the tempo and no beat: until a Start, the beats are
            // nobody's, and DOWNBEAT is what puts them on the music.
            view.note = "no start received \xE2\x80\x94 press downbeat on the beat";
        }
    }
    view.stripTicks = facts.ticking && !facts.coasting ? static_cast<int>(facts.tickInBeat) + 1 : 0;
    return view;
}

void publishTimingView(MainWindow& window, const TimingView& view) {
    window.set_timing_index(view.index);
    window.set_timing_name(slint::SharedString(view.name));
    window.set_timing_short(slint::SharedString(view.shortName));
    window.set_timing_reading(slint::SharedString(view.reading));
    window.set_timing_detail(slint::SharedString(view.detail));
    window.set_timing_note(slint::SharedString(view.note));
    window.set_timing_waiting(view.waiting);
    window.set_timing_waiting_word(slint::SharedString(view.waitingWord));
    window.set_timing_coasting(view.coasting);
    window.set_strip_beat(view.stripBeat);
    window.set_strip_ticks(view.stripTicks);
    window.set_strip_label(slint::SharedString(view.stripLabel));
    window.set_timing_network(slint::SharedString(view.network));
    window.set_timing_beats_per_bar(view.beatsPerBar);
    window.set_clock_obey(view.obey);
    window.set_timing_ready(view.ready);
}

void publishClockPorts(MainWindow& window, const std::vector<std::string>& inputs,
                       const std::string& wanted) {
    std::vector<std::string> choices = inputs;
    int picked = 0;
    if (!wanted.empty()) {
        // By the rule the clock finds its port by, so a name saved from another numbering of the
        // same device is that device; one not on this machine stays on the list, said to be gone.
        if (const std::optional<std::size_t> found = output::findMidiPort(inputs, wanted)) {
            picked = static_cast<int>(*found) + 1;
        } else {
            choices.push_back(wanted);
            picked = static_cast<int>(choices.size());
        }
    }
    auto ports = std::make_shared<slint::VectorModel<slint::SharedString>>();
    ports->push_back(
        slint::SharedString(inputs.empty() ? "no MIDI inputs on this machine" : "select input"));
    for (std::size_t i = 0; i < choices.size(); ++i) {
        ports->push_back(slint::SharedString(
            i < inputs.size() ? choices[i] : choices[i] + " \xE2\x80\x94 not plugged in"));
    }
    window.set_clock_in_ports(ports);
    window.set_clock_in_port_index(picked);
}

std::vector<std::uint8_t>
publishCdjFollow(MainWindow& window, const std::vector<CdjPlayer>& players, std::uint8_t follow) {
    std::vector<std::uint8_t> numbers{0};
    auto choices = std::make_shared<slint::VectorModel<slint::SharedString>>();
    choices->push_back(slint::SharedString("tempo master"));
    int picked = 0;
    for (const CdjPlayer& player : players) {
        numbers.push_back(player.number);
        std::string text = "player " + std::to_string(player.number);
        if (!player.name.empty()) {
            text += " \xC2\xB7 " + player.name;
        }
        choices->push_back(slint::SharedString(text));
        if (player.number == follow) {
            picked = static_cast<int>(numbers.size()) - 1;
        }
    }
    if (follow != 0 && picked == 0) {
        // Asked for and not on the network now: kept, and said to be missing.
        numbers.push_back(follow);
        choices->push_back(
            slint::SharedString("player " + std::to_string(follow) + " \xE2\x80\x94 not heard"));
        picked = static_cast<int>(numbers.size()) - 1;
    }
    window.set_cdj_follow_choices(choices);
    window.set_cdj_follow_index(picked);
    return numbers;
}

void publishControlLimits(MainWindow& window) {
    window.set_fold_limit_min(static_cast<float>(kFoldFloorBpm));
    window.set_fold_limit_max(static_cast<float>(kFoldCeilingBpm));
    window.set_fold_least_span(static_cast<float>(kFoldLeastSpanBpm));
    window.set_latency_limit_ms(static_cast<float>(kLatencyLimitMs));
    // Per-output delay, which is a different control from the latency above: that one
    // moves the whole rig's timeline, this one is one cable's own lag.
    window.set_output_delay_limit_ms(static_cast<float>(output::kMaxOutputDelaySeconds * 1000.0));
    // What a right-click puts the tempo window and the latency back to: a fresh preset's.
    const tracking::TempoTracker::Options fresh = settings::freshTempoOptions();
    window.set_fold_default_min(static_cast<float>(fresh.minBpm));
    window.set_fold_default_max(static_cast<float>(fresh.maxBpm));
    window.set_latency_default_ms(static_cast<float>(fresh.latencyOffsetSeconds * 1000.0));
}

void publishIdleReadouts(MainWindow& window) {
    publishTempoState(window, tracking::TempoState{});
    window.set_input_level(0.0f);
    window.set_input_peak(0.0f);
    window.set_input_reading(slint::SharedString(""));
}

void publishInput(MainWindow& window, float rms, float peak) {
    const float db = audio::toDbfs(rms);
    window.set_input_level(std::clamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f));
    window.set_input_peak(peak);
    window.set_input_reading(slint::SharedString(
        db <= kMeterFloorDb ? "-inf dB" : fixed(static_cast<double>(db), 1) + " dB"));
}

void publishGlobalMessages(MainWindow& window, const std::vector<OutputRow>& rows) {
    int osc = 0;
    int ticked = 0;
    std::string names;
    for (const OutputRow& row : rows) {
        if (row.kind_index != static_cast<int>(output::OutputTarget::Kind::Osc)) {
            continue;
        }
        ++osc;
        if (row.sends_namespace) {
            ++ticked;
            // As the list names it: the row's name, or its address where it has none.
            const std::string name = !row.name.empty()
                                         ? std::string(row.name)
                                         : std::string(row.host) + ":" + std::string(row.port);
            names += (names.empty() ? "" : ", ") + name;
        }
    }
    window.set_global_osc_count(osc);
    window.set_global_ticked(ticked);
    window.set_global_summary(slint::SharedString(ticked == 0 ? std::string("nothing")
                                                  : ticked == osc && osc > 1 ? "every OSC output"
                                                                             : names));
}

std::string describeDevice(const audio::InputDevice& device) {
    std::string text = device.hostApiName + " / " + device.name + "  (" +
                       std::to_string(device.maxInputChannels) + " in";
    if (device.isLoopback) {
        text += ", loopback";
    }
    return text + ")";
}

std::string describeChannel(const audio::InputDevice& device, int channel) {
    const std::string number = std::to_string(channel + 1);
    const auto index = static_cast<std::size_t>(channel);
    if (index < device.channelNames.size() && !device.channelNames[index].empty()) {
        return "In " + number + " — " + device.channelNames[index];
    }
    return "In " + number;
}

std::string describePair(const audio::InputDevice& device, int first) {
    std::string text =
        "In " + std::to_string(first + 1) + " + " + std::to_string(first + 2);
    const auto a = static_cast<std::size_t>(first);
    const auto b = a + 1;
    if (b < device.channelNames.size() && !device.channelNames[a].empty() &&
        !device.channelNames[b].empty()) {
        text += " — " + device.channelNames[a] + " / " + device.channelNames[b];
    }
    return text;
}

} // namespace takt4::ui
