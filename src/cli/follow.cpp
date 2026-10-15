#include "cli/follow.hpp"

#include "core/engine/beat_source.hpp"
#include "core/engine/control.hpp"
#include "core/output/link_peers.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/output_target.hpp"
#include "core/output/transports.hpp"
#include "core/timing/beat_grid.hpp"
#include "core/timing/external_beats.hpp"
#include "core/timing/link_follower.hpp"

#include "cli/args.hpp"
#include "cli/console.hpp"

#include <chrono>
#include <cstdint>
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
    out << "  takt4-cli follow --link [--beats-per-bar N] [--latency MS]\n"
        << "                   [--osc HOST:PORT] [--osc-prefix /NAME] [--midi-clock-out PORT]\n"
        << "                   [--seconds S]\n"
        << "      Take the beats from somewhere else instead of from audio, and send them on\n"
        << "      through the same outputs as `track`. Prints a line per beat.\n"
        << "      --link          follow the Ableton Link session on this network. Link stays\n"
        << "                      off and takt4 listens until a session has been heard for a\n"
        << "                      second, then joins it as its youngest peer, so it never\n"
        << "                      hands the session a tempo of its own; it leaves the moment\n"
        << "                      the last peer does, and the beats coast for four seconds\n"
        << "      --beats-per-bar N  the bar, default 4, and Link's quantum: Link carries no\n"
        << "                      meter, and peers align their bars at the same quantum\n"
        << "      --latency MS    move every output by this much; negative fires early\n"
        << "      --osc, --osc-prefix  as `track`\n"
        << "      --midi-clock-out P  24 PPQN to the MIDI output port named P, regenerated\n"
        << "                      from the followed beats\n"
        << "      --seconds S     stop after S seconds instead of on Ctrl-C\n"
        << "      Keys, in a console: d snaps the bar to the nearest beat, h and x halve and\n"
        << "      double the followed grid, p is PANIC, q stops.\n"
        << "\n";
}

namespace {

using namespace std::chrono_literals;

struct FollowArgs {
    bool link = false;
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
        if (arg == "--link") {
            out.link = true;
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
    if (!out.link) {
        throw std::invalid_argument("follow: say what to follow — --link");
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

std::string statusOf(const timing::LinkFollower& follower, const timing::ExternalBeats& beats) {
    const timing::LinkFollower::Status status = follower.status();
    const tracking::TempoState state = beats.state();
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
    // Following Link, the outputs do not lead it: the session is handed over switched off before
    // anything follows it. The runner is not running yet, so this takes effect here and now.
    runner.post(output::OutputCommand::linkFollowing(true));
    // The follower reads the outputs' own session — two Link instances in one process would be
    // two peers — and listens to Link's discovery traffic for one to join.
    output::LinkPeerWatch listener;
    timing::LinkFollower follower(runner.transports().link(), listener);
    // And the timing worker, which reads the follower, stopped before either goes.
    struct StopFirst {
        timing::ExternalBeats& beats;
        ~StopFirst() { beats.stop(); }
    } stopFirst{beats};

    std::cout << "following: Ableton Link — Link stays off until a session has been heard for a\n"
              << "           second, so takt4 joins it and never hands it a tempo of its own\n"
              << "bar:       " << parsed.beatsPerBar << " beats, which is Link's quantum\n"
              << "latency:   " << fixed(parsed.latencySeconds * 1000.0, 1)
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
    beats.start(follower, timing::BeatGrid::Options{parsed.beatsPerBar, 4.0});

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
        // What the follower is doing, whenever it changes, and every few seconds while it waits
        // for a session — a console with nothing on it looks like a console that has hung.
        const std::string status = statusOf(follower, beats);
        if (status != lastStatus || (!beats.state().locked && elapsed() - lastStatusAt >= 5.0)) {
            std::cout << "  " << fixed(elapsed(), 1, 8) << "s  " << status << '\n';
            lastStatus = status;
            lastStatusAt = elapsed();
        }
        std::cout << std::flush;
    }
    beats.stop();  // Link off, and the listening stopped
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
