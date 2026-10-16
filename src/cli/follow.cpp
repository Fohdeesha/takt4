#include "cli/follow.hpp"

#include "core/engine/beat_source.hpp"
#include "core/engine/control.hpp"
#include "core/midi/midi_input_hub.hpp"
#include "core/output/link_peers.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/midi_ports.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/output_target.hpp"
#include "core/output/transports.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/external_beats.hpp"
#include "core/timing/grid_source.hpp"
#include "core/timing/link_follower.hpp"
#include "core/timing/link_reader.hpp"
#include "core/timing/midi_clock_follower.hpp"
#include "core/timing/prolink/network.hpp"
#include "core/timing/prolink/virtual_player.hpp"
#include "core/timing/prolink_follower.hpp"

#include "cli/args.hpp"
#include "cli/console.hpp"
#include "cli/prolink_watch.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace takt4::cli {

void printFollowUsage(std::ostream& out) {
    out << "  takt4-cli follow (--link | --midi-clock PORT [--obey-start-stop] | --metronome BPM\n"
        << "                   | --prolink [--follow master|N] [--interface ADDRESS])\n"
        << "                   [--beats-per-bar N] [--latency MS] [--osc HOST:PORT]\n"
        << "                   [--osc-prefix /NAME] [--midi-clock-out PORT] [--out FILE]\n"
        << "                   [--seconds S]\n"
        << "      Take the beats from somewhere else instead of from audio, and send them on\n"
        << "      through the same outputs as `track`. Prints a line per beat.\n"
        << "      --link          follow the Ableton Link session on this network. Link stays\n"
        << "                      off and takt4 listens until a session has been heard for a\n"
        << "                      second, then joins it as its youngest peer, so it never\n"
        << "                      hands the session a tempo of its own; it leaves the moment\n"
        << "                      the last peer does, and the beats coast for four seconds\n"
        << "      --midi-clock P  follow the MIDI clock arriving on input port P (part of its\n"
        << "                      name, or its index: `midi-in` lists them). Its tempo is\n"
        << "                      measured from two beats of ticks; Start, Continue and Song\n"
        << "                      Position say where bar 1 is, and until one comes d puts the\n"
        << "                      beat under the key\n"
        << "      --obey-start-stop  a MIDI Stop halts the beats at once, until Start or\n"
        << "                      Continue. Without it a Stop is let pass, as a DJ mixer's\n"
        << "                      clock runs on after its Stop button\n"
        << "      --prolink       follow the CDJs on this network (Pro DJ Link; experimental,\n"
        << "                      not yet tried on real CDJs): takt4 joins as player 7 to 15\n"
        << "                      and takes each beat the tempo master sends, its bar the\n"
        << "                      DJ's beat grid's, four to the bar. Cue presses are not\n"
        << "                      beats. rekordbox on this computer holds the same ports\n"
        << "      --follow WHO    with --prolink: `master` (the default) or a player's number\n"
        << "      --interface A   with --prolink: join on the network card with address A\n"
        << "      --metronome BPM  follow a steady grid at BPM on this machine's clock: a\n"
        << "                      reference to measure the outputs by, a MIDI clock above all\n"
        << "      --beats-per-bar N  the bar, default 4, and Link's quantum: neither Link nor a\n"
        << "                      MIDI clock carries a meter\n"
        << "      --latency MS    move every output by this much; negative fires early\n"
        << "      --osc, --osc-prefix  as `track`\n"
        << "      --midi-clock-out P  24 PPQN to the MIDI output port named P, regenerated\n"
        << "                      from the followed beats; refused to the device a followed\n"
        << "                      MIDI clock comes from, which could send it back round\n"
        << "      --out FILE      every beat sent, one a line: its moment in microseconds on\n"
        << "                      Link's clock, a tab, its place in the bar, a tab, the tempo\n"
        << "      --seconds S     stop after S seconds instead of on Ctrl-C\n"
        << "      Keys, in a console: d snaps the bar to the nearest beat, h and x halve and\n"
        << "      double the followed grid, p is PANIC, q stops.\n"
        << "\n";
}

namespace {

using namespace std::chrono_literals;

/// What `follow` takes its beats from.
enum class Followed { None, Link, MidiClock, Metronome, ProLink };

struct FollowArgs {
    Followed followed = Followed::None;
    std::string midiClockIn;
    bool obeyStartStop = false;
    double metronomeBpm = 0.0;
    std::uint8_t proLinkFollow = timing::prolink::VirtualPlayer::kFollowMaster;
    bool proLinkFollowGiven = false;
    std::optional<std::uint32_t> proLinkInterface;
    std::optional<std::string> out;
    std::uint32_t beatsPerBar = 4;
    double latencySeconds = 0.0;
    std::string oscPrefix = "/takt4";
    std::vector<std::pair<std::string, std::uint16_t>> oscTargets;
    std::optional<std::string> midiClockOut;
    double seconds = 0.0; // 0: until interrupted
};

FollowArgs parseFollowArgs(const std::vector<std::string_view>& args) {
    FollowArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        const auto follow = [&out](Followed followed) {
            if (out.followed != Followed::None) {
                throw std::invalid_argument("follow takes its beats from one source at a time");
            }
            out.followed = followed;
        };
        if (arg == "--link") {
            follow(Followed::Link);
        } else if (arg == "--midi-clock") {
            follow(Followed::MidiClock);
            out.midiClockIn = std::string(value());
        } else if (arg == "--obey-start-stop") {
            out.obeyStartStop = true;
        } else if (arg == "--prolink") {
            follow(Followed::ProLink);
        } else if (arg == "--follow") {
            out.proLinkFollow = parseProLinkFollow(value());
            out.proLinkFollowGiven = true;
        } else if (arg == "--interface") {
            const std::string text(value());
            out.proLinkInterface = timing::prolink::parseAddress(text);
            if (!out.proLinkInterface) {
                throw std::invalid_argument("--interface expects an IPv4 address: \"" + text +
                                            "\"");
            }
        } else if (arg == "--out") {
            out.out = std::string(value());
        } else if (arg == "--metronome") {
            follow(Followed::Metronome);
            out.metronomeBpm = parseDouble(value(), arg);
            if (!(out.metronomeBpm >= 20.0 && out.metronomeBpm <= 400.0)) {
                throw std::invalid_argument("--metronome expects 20 to 400 BPM");
            }
        } else if (arg == "--beats-per-bar") {
            const int beats = parseInt(value(), arg);
            if (beats < 1 || beats > 16) {
                throw std::invalid_argument("--beats-per-bar expects 1 to 16");
            }
            out.beatsPerBar = static_cast<std::uint32_t>(beats);
        } else if (arg == "--latency") {
            out.latencySeconds = parseDouble(value(), arg) / 1000.0;
        } else if (arg == "--osc") {
            out.oscTargets.push_back(parseOscTarget(value(), arg));
        } else if (arg == "--osc-prefix") {
            out.oscPrefix = std::string(value());
        } else if (arg == "--midi-clock-out") {
            out.midiClockOut = std::string(value());
        } else if (arg == "--seconds") {
            out.seconds = parseDouble(value(), arg);
            if (!(out.seconds > 0.0)) {
                throw std::invalid_argument("--seconds expects a positive number");
            }
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    if (out.followed == Followed::None) {
        throw std::invalid_argument("follow: say what to follow — --link, --midi-clock PORT, "
                                    "--prolink or --metronome BPM");
    }
    if ((out.proLinkFollowGiven || out.proLinkInterface) && out.followed != Followed::ProLink) {
        throw std::invalid_argument("--follow and --interface are for --prolink");
    }
    if (out.followed == Followed::ProLink && out.beatsPerBar != 4) {
        throw std::invalid_argument("--beats-per-bar: the CDJs count four to the bar");
    }
    if (out.obeyStartStop && out.followed != Followed::MidiClock) {
        throw std::invalid_argument("--obey-start-stop is for a followed MIDI clock");
    }
    return out;
}

std::string fixed(double value, int places, int width = 0) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(places) << std::setw(width) << value;
    return text.str();
}

std::string beatLine(const engine::EngineBeat& beat, bool coasting) {
    std::ostringstream line;
    line << fixed(beat.event.time, 3, 9) << "s  " << (beat.event.downbeat ? "DOWNBEAT" : "beat    ")
         << "  ";
    if (beat.event.beatsPerBar > 0 && beat.event.beatInBar > 0) {
        for (std::uint32_t b = 1; b <= beat.event.beatsPerBar; ++b) {
            line << (b == beat.event.beatInBar ? '#' : '.');
        }
        line << "  " << beat.event.beatInBar << "/" << beat.event.beatsPerBar;
    } else {
        line << "?    ";
    }
    line << "  " << fixed(beat.event.bpm, 2, 7) << " BPM  "
         << (coasting ? "coasting " : "following");
    if (beat.state.octaveShift < 0) {
        line << "  (÷" << (1 << -beat.state.octaveShift) << ")";
    } else if (beat.state.octaveShift > 0) {
        line << "  (×" << (1 << beat.state.octaveShift) << ")";
    }
    if (beat.event.snapped) {
        line << "  (bar moved)";
    }
    return line.str();
}

/// A grid at one tempo, from the moment a run begins, on Link's clock: what takt4's outputs are
/// measured against — nothing it is following can be steadier.
class Metronome final : public timing::GridSource {
public:
    Metronome(const timing::LinkReader& clock, double bpm) : clock_(clock), bpm_(bpm) {}
    void begin(std::uint32_t) noexcept override { startedAt_ = clock_.now().count(); }
    timing::GridReading read() noexcept override {
        timing::GridReading reading;
        reading.at = clock_.now().count();
        reading.present = true;
        reading.bpm = bpm_;
        reading.beat = static_cast<double>(reading.at - startedAt_) * bpm_ / 60e6;
        reading.anchored = true;
        return reading;
    }
    void end() noexcept override {}

private:
    const timing::LinkReader& clock_;
    double bpm_;
    std::int64_t startedAt_ = 0;
};

std::string linkStatus(const timing::LinkFollower& follower) {
    const timing::LinkFollower::Status status = follower.status();
    std::ostringstream line;
    switch (status.phase) {
    case timing::LinkFollower::Phase::Listening:
        if (!status.canListen) {
            line << "cannot listen for Link: " << follower.problem();
        } else if (status.peers == 0) {
            line << "listening: no Link session heard on this network";
        } else {
            line << "listening: " << status.peers << (status.peers == 1 ? " peer" : " peers")
                 << " heard, at " << fixed(status.bpm, 2) << " BPM";
        }
        break;
    case timing::LinkFollower::Phase::Joining:
        line << "joining the session heard, at " << fixed(status.bpm, 2) << " BPM";
        break;
    case timing::LinkFollower::Phase::Following:
        line << "following: " << status.peers << (status.peers == 1 ? " peer" : " peers")
             << " in the session, at " << fixed(status.bpm, 2) << " BPM";
        break;
    }
    return line.str();
}

/// Without the tempo, which a measured clock moves in its last decimal from one look to the next:
/// the beat lines carry it.
std::string midiClockStatus(const timing::MidiClockFollower& follower, const std::string& port) {
    const timing::MidiClockFollower::Status status = follower.status();
    std::ostringstream line;
    line << "MIDI clock on \"" << port << "\": ";
    if (!status.ticking) {
        line << "no ticks arriving";
    } else if (status.bpm == 0.0) {
        line << "ticking, measuring the tempo";
    } else {
        line << "following";
    }
    if (status.stopped) {
        line << (follower.obeyStartStop() ? ", stopped: waiting for Start or Continue"
                                          : ", Stop received and let pass");
    } else if (status.ticking && !status.placed) {
        line << ", no Start received: press d on the beat";
    }
    return line.str();
}

/// A MIDI clock sent to the device the clock is followed from could come straight back round —
/// through its MIDI thru, or a virtual port's own loop — and be followed: refused, before
/// anything is opened. The device is the port's name without the number Windows gives it.
void refuseClockLoop(const std::string& inputSpec, const std::string& outputSpec) {
    const std::vector<std::string> inputs = output::listMidiInputPorts();
    const std::vector<std::string> outputs = output::listMidiOutputPorts();
    const std::optional<std::size_t> in = output::findMidiPort(inputs, inputSpec);
    const std::optional<std::size_t> out = output::findMidiPort(outputs, outputSpec);
    if (in && out &&
        output::midiPortBaseName(inputs[*in]) == output::midiPortBaseName(outputs[*out])) {
        throw std::invalid_argument("--midi-clock-out \"" + outputs[*out] +
                                    "\" is the device the followed clock comes from (\"" +
                                    inputs[*in] + "\"), and could send it straight back round");
    }
}

/// What the source is doing, and then what the beats are.
std::string statusOf(const std::string& source, const timing::ExternalBeats& beats) {
    const tracking::TempoState state = beats.state();
    std::ostringstream line;
    line << source;
    if (state.noSignal) {
        line << " — no signal: the outputs let go";
    } else if (beats.coasting()) {
        line << " — coasting at the last tempo";
    }
    return line.str();
}

void printKeys(std::ostream& out, bool available) {
    if (!available) {
        out << "keys:      not available — stdin is not a console\n";
        return;
    }
    out << "keys:      d downbeat now · h /2 · x *2 · p PANIC, again lets go · q quit\n";
}

} // namespace

int runFollow(const std::vector<std::string_view>& args) {
    const FollowArgs parsed = parseFollowArgs(args);
    if (parsed.followed == Followed::MidiClock && parsed.midiClockOut) {
        refuseClockLoop(parsed.midiClockIn, *parsed.midiClockOut);
    }
    std::ofstream out;
    if (parsed.out) {
        out.open(*parsed.out, std::ios::trunc);
        if (!out) {
            throw std::runtime_error(*parsed.out + ": cannot create");
        }
    }
    output::Transports::Config config;
    config.oscPrefix = parsed.oscPrefix;
    config.outputs = output::oscOutputs(parsed.oscTargets);
    config.midiClockPort = parsed.midiClockOut;
    config.latencySeconds = parsed.latencySeconds;

    // **The declaration order from here down is the destruction order reversed**, as in `track`:
    // it is what keeps every thread from outliving what it reads when the scope unwinds.
    //
    // The beats first: the output thread drains them.
    timing::ExternalBeats beats;
    // Then what the output thread hands over, which its observer holds a reference to.
    std::mutex publishedMutex;
    std::vector<engine::EngineBeat> published;
    output::OutputRunner runner(beats, config);
    const output::Transports& transports = runner.transports();
    // Then the source, which the timing worker reads. Link is followed through the outputs' own
    // session — two Link instances in one process would be two peers — and its discovery
    // traffic is listened to for a session to join. A MIDI clock's messages are stamped on
    // Link's clock as they arrive, as everything followed is; the metronome only reads it.
    std::optional<output::LinkPeerWatch> listener;
    std::optional<timing::LinkFollower> linkFollower;
    std::optional<midi::MidiInputHub> inputs;
    std::optional<timing::MidiClockFollower> clockFollower;
    std::string clockPort;
    std::optional<Metronome> metronome;
    std::optional<timing::prolink::Network> cdjs;
    std::optional<timing::ProLinkFollower> cdjFollower;
    timing::GridSource* source = nullptr;
    switch (parsed.followed) {
    case Followed::Link:
        // Following Link, the outputs do not lead it: the session is handed over switched off
        // before anything follows it. The runner is not running yet, so this takes effect here
        // and now.
        runner.post(output::OutputCommand::linkFollowing(true));
        listener.emplace();
        linkFollower.emplace(runner.transports().link(), *listener);
        source = &*linkFollower;
        break;
    case Followed::MidiClock:
        inputs.emplace([&transports] { return transports.link().now().count(); });
        clockFollower.emplace(*inputs);
        clockFollower->setObeyStartStop(parsed.obeyStartStop);
        clockPort = clockFollower->listen(parsed.midiClockIn);
        source = &*clockFollower;
        break;
    case Followed::ProLink: {
        timing::prolink::Network::Config cdjConfig;
        cdjConfig.clock = [&transports] { return transports.link().now().count(); };
        cdjConfig.interfaceAddress = parsed.proLinkInterface;
        cdjs.emplace(cdjConfig);
        cdjs->setFollow(parsed.proLinkFollow);
        std::string problem;
        if (!cdjs->open(problem)) {
            throw std::runtime_error(problem);
        }
        cdjFollower.emplace(*cdjs);
        source = &*cdjFollower;
        break;
    }
    case Followed::Metronome:
    case Followed::None:
        metronome.emplace(runner.transports().link(), parsed.metronomeBpm);
        source = &*metronome;
        break;
    }
    // And the timing worker, which reads the source, stopped before it goes.
    struct StopFirst {
        timing::ExternalBeats& beats;
        ~StopFirst() { beats.stop(); }
    } stopFirst{beats};
    const auto sourceStatus = [&]() -> std::string {
        if (linkFollower) {
            return linkStatus(*linkFollower);
        }
        if (clockFollower) {
            return midiClockStatus(*clockFollower, clockPort);
        }
        if (cdjs) {
            return proLinkStatus(cdjs->snapshot());
        }
        return "metronome at " + fixed(parsed.metronomeBpm, 2) + " BPM";
    };

    if (parsed.followed == Followed::Link) {
        std::cout
            << "following: Ableton Link — Link stays off until a session has been heard for a\n"
            << "           second, so takt4 joins it and never hands it a tempo of its own\n"
            << "bar:       " << parsed.beatsPerBar << " beats, which is Link's quantum\n";
    } else if (parsed.followed == Followed::MidiClock) {
        std::cout << "following: the MIDI clock on \"" << clockPort << "\" — the tempo from two\n"
                  << "           beats of ticks, bar 1 from Start, Continue or Song Position\n"
                  << "stop:      "
                  << (parsed.obeyStartStop ? "a MIDI Stop halts the beats until Start or Continue"
                                           : "a MIDI Stop is let pass (--obey-start-stop to obey)")
                  << '\n'
                  << "bar:       " << parsed.beatsPerBar << " beats\n";
    } else if (parsed.followed == Followed::ProLink) {
        std::cout << "following: the CDJs (Pro DJ Link, experimental) — "
                  << (parsed.proLinkFollow == timing::prolink::VirtualPlayer::kFollowMaster
                          ? std::string("the tempo master")
                          : "player " + std::to_string(parsed.proLinkFollow))
                  << ", takt4 joining as a player\n"
                  << "bar:       4 beats, the DJ's beat grid's\n";
    } else {
        std::cout << "following: a metronome at " << fixed(parsed.metronomeBpm, 3)
                  << " BPM on this machine's clock\n"
                  << "bar:       " << parsed.beatsPerBar << " beats\n";
    }
    std::cout << "latency:   " << fixed(parsed.latencySeconds * 1000.0, 1)
              << " ms on every output\n"
              << "outputs:   ";
    if (transports.osc().targetCount() == 0 && transports.midiPort() == nullptr) {
        std::cout << "none (--osc HOST:PORT, --midi-clock-out PORT)";
    }
    for (std::size_t i = 0; i < transports.osc().targetCount(); ++i) {
        std::cout << "OSC " << transports.osc().target(i).resolved() << " ";
    }
    if (transports.midiPort() != nullptr) {
        std::cout << "MIDI clock to \"" << transports.midiPort()->portName() << "\"";
    }
    std::cout << '\n';
    KeyReader keys;
    printKeys(std::cout, keys.interactive());
    const Interrupts interrupts;

    runner.setBeatObserver([&](const engine::EngineBeat& beat) {
        const std::lock_guard<std::mutex> lock(publishedMutex);
        published.push_back(beat);
    });
    runner.start();
    // Listening from the start, which is what gives the MIDI clock its Start.
    runner.setTracking(true);
    beats.start(*source, timing::BeatGrid::Options{parsed.beatsPerBar, 4.0});

    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&start] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    bool quit = false;
    const auto onKey = [&](int key) {
        switch (key) {
        case 'd':
            (void)beats.post(engine::Command::snapDownbeat());
            std::cout << "  downbeat: the nearest beat starts the bar\n";
            break;
        case 'h':
            (void)beats.post(engine::Command::halve());
            std::cout << "  /2\n";
            break;
        case 'x':
            (void)beats.post(engine::Command::redouble());
            std::cout << "  *2\n";
            break;
        case 'p': {
            const bool halt = !runner.panicked();
            runner.panic(halt);
            std::cout << (halt ? "  PANIC: every rule halted\n" : "  panic released\n");
            break;
        }
        case '?':
            printKeys(std::cout, true);
            break;
        case 'q':
            quit = true; // the same way out as Ctrl+C
            break;
        default:
            break;
        }
    };

    std::string lastStatus;
    double lastStatusAt = -10.0;
    std::vector<engine::EngineBeat> toPrint;
    const auto printBeats = [&] {
        {
            const std::lock_guard<std::mutex> lock(publishedMutex);
            toPrint.swap(published);
        }
        for (const engine::EngineBeat& beat : toPrint) {
            std::cout << beatLine(beat, beats.coasting()) << '\n';
            if (out.is_open()) {
                out << beat.hostMicros << '\t' << beat.event.beatInBar << '\t'
                    << fixed(beat.event.bpm, 4) << '\n';
            }
        }
        toPrint.clear();
    };
    while (!quit && !Interrupts::requested() &&
           !(parsed.seconds > 0.0 && elapsed() >= parsed.seconds)) {
        std::this_thread::sleep_for(10ms);
        for (int key = keys.poll(); key != 0; key = keys.poll()) {
            onKey(key);
        }
        printBeats();
        // What the source is doing, whenever it changes, and every few seconds while it waits
        // for a session — a console with nothing on it looks like a console that has hung.
        const std::string status = statusOf(sourceStatus(), beats);
        if (status != lastStatus || (!beats.state().locked && elapsed() - lastStatusAt >= 5.0)) {
            std::cout << "  " << fixed(elapsed(), 1, 8) << "s  " << status << '\n';
            lastStatus = status;
            lastStatusAt = elapsed();
        }
        std::cout << std::flush;
    }
    beats.stop();  // Link off, and the listening stopped, if it was Link
    runner.stop(); // the last beats, the releases owed, a MIDI clock's Stop
    printBeats();
    std::cout << "stopped after " << fixed(elapsed(), 1) << " s: " << transports.beats()
              << " beats (" << transports.downbeats() << " downbeats) sent";
    if (beats.commandsRefused() != 0) {
        std::cout << ", " << beats.commandsRefused() << " commands refused";
    }
    std::cout << '\n';
    if (transports.osc().targetCount() != 0) {
        std::cout << "OSC: " << transports.osc().messagesSent() << " messages sent, "
                  << transports.osc().messagesFailed() << " failed\n";
    }
    if (transports.midiClock() != nullptr) {
        std::cout << "MIDI clock: " << transports.midiClock()->ticksSent() << " ticks, "
                  << transports.midiClock()->ticksSkipped() << " skipped\n";
    }
    std::cout << std::flush;
    return 0;
}

} // namespace takt4::cli
