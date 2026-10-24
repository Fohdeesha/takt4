#include "cli/prolink_watch.hpp"

#include "core/output/link_session.hpp"
#include "core/timing/prolink/network.hpp"
#include "core/timing/prolink/virtual_player.hpp"

#include "cli/args.hpp"
#include "cli/console.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace takt4::cli {

using timing::prolink::DeviceKind;
using timing::prolink::Network;
using timing::prolink::VirtualPlayer;

void printProLinkWatchUsage(std::ostream& out) {
    out << "  takt4-cli prolink-watch [--join] [--follow master|N] [--interface ADDRESS]\n"
        << "                          [--seconds S]\n"
        << "      List the Pro DJ Link devices on this network — CDJs, a DJM, rekordbox —\n"
        << "      as they are heard, sending nothing. Experimental: not yet tried on real\n"
        << "      CDJs. rekordbox on this computer holds the same ports.\n"
        << "      --join          announce takt4 as a player (7 to 15, the first free) to\n"
        << "                      hear each device's status — playing, tempo master, tempo\n"
        << "                      — and print the beats of the device followed\n"
        << "      --follow WHO    `master` (the default: the tempo master, or the one player\n"
        << "                      playing while there is none) or a player's number\n"
        << "      --interface A   join on the network card with address A; by default the\n"
        << "                      one the first device heard is on\n"
        << "      --seconds S     stop after S seconds instead of on Ctrl-C\n"
        << "\n";
}

std::uint8_t parseProLinkFollow(std::string_view text) {
    if (text == "master") {
        return VirtualPlayer::kFollowMaster;
    }
    const int player = parseInt(text, "--follow");
    if (player < 1 || player > 15) {
        throw std::invalid_argument("--follow expects master or a player's number, 1 to 15");
    }
    return static_cast<std::uint8_t>(player);
}

namespace {

std::string fixed(double value, int places) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(places) << value;
    return text.str();
}

const char* phaseText(VirtualPlayer::Phase phase) {
    switch (phase) {
    case VirtualPlayer::Phase::Listening:
        return "listening";
    case VirtualPlayer::Phase::Watching:
        return "watching the network before claiming a number";
    case VirtualPlayer::Phase::Hello:
    case VirtualPlayer::Phase::FirstClaim:
    case VirtualPlayer::Phase::SecondClaim:
    case VirtualPlayer::Phase::FinalClaim:
        return "claiming a player number";
    case VirtualPlayer::Phase::AwaitingMixer:
        return "waiting for the mixer to assign a number";
    case VirtualPlayer::Phase::Joined:
        return "joined";
    case VirtualPlayer::Phase::NoNumber:
        return "every player number from 7 to 15 is taken";
    }
    return "?";
}

struct WatchArgs {
    bool join = false;
    std::uint8_t follow = VirtualPlayer::kFollowMaster;
    std::optional<std::uint32_t> interfaceAddress;
    double seconds = 0.0;
};

WatchArgs parseWatchArgs(const std::vector<std::string_view>& args) {
    WatchArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--join") {
            out.join = true;
        } else if (arg == "--follow") {
            out.follow = parseProLinkFollow(value());
        } else if (arg == "--interface") {
            const std::string text(value());
            out.interfaceAddress = timing::prolink::parseAddress(text);
            if (!out.interfaceAddress) {
                throw std::invalid_argument("--interface expects an IPv4 address: \"" + text +
                                            "\"");
            }
        } else if (arg == "--seconds") {
            out.seconds = parseDouble(value(), arg);
            if (!(out.seconds > 0.0)) {
                throw std::invalid_argument("--seconds expects a positive number");
            }
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    return out;
}

} // namespace

std::string proLinkDevice(const Network::DeviceView& device) {
    std::ostringstream line;
    const char* kind = device.kind == DeviceKind::Mixer    ? "mixer "
                       : device.kind == DeviceKind::Player ? "player"
                                                           : "device";
    line << kind << ' ' << std::setw(2) << static_cast<int>(device.number) << "  " << std::left
         << std::setw(20) << device.name << std::right << "  "
         << timing::prolink::addressText(device.ip);
    if (device.hasStatus) {
        line << "  " << (device.playing ? "playing" : "stopped");
        if (device.master) {
            line << (device.masterWithoutBeats ? ", master (a track that cannot lead)"
                                               : ", master");
        }
        if (device.synced) {
            line << ", synced";
        }
        if (device.onAir) {
            line << ", on the air";
        }
        if (device.cueing) {
            line << ", cueing";
        }
        if (device.bpm > 0.0) {
            line << "  " << fixed(device.bpm, 2) << " BPM";
        }
    }
    if (device.beating) {
        line << "  beating";
    }
    return line.str();
}

std::string proLinkStatus(const Network::Snapshot& snapshot) {
    std::ostringstream line;
    if (!snapshot.problem.empty()) {
        return "CDJs: " + snapshot.problem;
    }
    if (!snapshot.open) {
        return "CDJs: not listening";
    }
    std::size_t players = 0;
    std::size_t mixers = 0;
    std::size_t leaders = 0;
    for (const Network::DeviceView& device : snapshot.devices) {
        players += device.kind == DeviceKind::Player ? 1u : 0u;
        mixers += device.kind == DeviceKind::Mixer ? 1u : 0u;
        leaders += device.kind == DeviceKind::Player && device.canLead ? 1u : 0u;
    }
    line << "CDJs: " << players << (players == 1 ? " player" : " players");
    if (mixers != 0) {
        line << ", " << mixers << (mixers == 1 ? " mixer" : " mixers");
    }
    line << " heard; " << phaseText(snapshot.phase);
    if (snapshot.phase == VirtualPlayer::Phase::Joined) {
        line << " as player " << static_cast<int>(snapshot.number) << " on "
             << timing::prolink::addressText(snapshot.interfaceIp);
        if (!snapshot.interfaceName.empty()) {
            line << " (" << snapshot.interfaceName << ")";
        }
    }
    if (snapshot.master != 0) {
        line << "; master " << static_cast<int>(snapshot.master);
    }
    if (snapshot.followed != 0) {
        line << "; following " << static_cast<int>(snapshot.followed);
    } else if (snapshot.phase == VirtualPlayer::Phase::Joined) {
        if (snapshot.follow != VirtualPlayer::kFollowMaster) {
            line << "; player " << static_cast<int>(snapshot.follow) << " not heard";
        } else if (leaders >= 2) {
            line << "; " << leaders << " decks playing and none is the master: --follow one";
        } else {
            line << (snapshot.onAirTold ? "; no deck playing on air to follow"
                                        : "; no deck playing to follow");
        }
    }
    if (!snapshot.note.empty()) {
        line << "; " << snapshot.note;
    }
    if (snapshot.sendsFailing) {
        line << "; sends failing, joining again";
    }
    if (snapshot.nothingHeard) {
        line << "; nothing at all coming in (a firewall may be blocking takt4)";
    }
    return line.str();
}

int runProLinkWatch(const std::vector<std::string_view>& args) {
    const WatchArgs parsed = parseWatchArgs(args);
    // Link's clock, which every beat takt4 follows is stamped on. Never switched on.
    output::LinkSession link(120.0);
    Network::Config config;
    config.clock = [&link] { return link.now().count(); };
    if (parsed.interfaceAddress) {
        config.interfacePick = timing::prolink::CardPick{*parsed.interfaceAddress, {}, {}};
    }
    Network network(config);
    network.setFollow(parsed.follow);
    std::string problem;
    if (!network.open(problem)) {
        throw std::runtime_error(problem);
    }
    std::cout << "listening for Pro DJ Link devices on UDP 50000"
              << (parsed.join ? "; joining as a player once one is heard"
                              : " — nothing is sent (--join to announce takt4)")
              << '\n';
    if (parsed.join) {
        network.join();
    }
    const Interrupts interrupts;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&start] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    std::string lastStatus;
    std::vector<std::string> lastDevices;
    double listedAt = -10.0;
    double cardCheckedAt = 0.0;
    std::int64_t firstBeat = 0;
    while (!Interrupts::requested() && !(parsed.seconds > 0.0 && elapsed() >= parsed.seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        // The card joined on, looked for every few seconds (`Network::recheckCard`).
        if (elapsed() - cardCheckedAt >= 5.0) {
            cardCheckedAt = elapsed();
            network.recheckCard();
        }
        VirtualPlayer::Heard beat;
        while (network.popBeat(beat)) {
            if (firstBeat == 0) {
                firstBeat = beat.at;
            }
            std::cout << "  " << fixed(static_cast<double>(beat.at - firstBeat) / 1e6, 3)
                      << "s  beat  player " << static_cast<int>(beat.device) << "  ";
            if (beat.beatInBar != 0) {
                std::cout << static_cast<int>(beat.beatInBar) << "/4";
            } else {
                std::cout << "?  ";
            }
            std::cout << "  " << fixed(beat.bpm, 2) << " BPM\n";
        }
        const Network::Snapshot snapshot = network.snapshot();
        const std::string status = proLinkStatus(snapshot);
        std::vector<std::string> devices;
        for (const Network::DeviceView& device : snapshot.devices) {
            devices.push_back(proLinkDevice(device));
        }
        if (status != lastStatus) {
            std::cout << "  " << fixed(elapsed(), 1) << "s  " << status << '\n';
            lastStatus = status;
        }
        // The devices, whenever they change, and every few seconds so a quiet console does not
        // look like one that has hung.
        if (devices != lastDevices || elapsed() - listedAt >= 5.0) {
            for (const std::string& device : devices) {
                std::cout << "            " << device << '\n';
            }
            if (devices.empty()) {
                std::cout << "            no device heard yet\n";
            }
            lastDevices = devices;
            listedAt = elapsed();
        }
        std::cout << std::flush;
    }
    network.leave();
    // A round of the network's thread, so the keep-alives have stopped before it is closed.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const Network::Snapshot last = network.snapshot();
    network.close();
    std::cout << "stopped after " << fixed(elapsed(), 1) << " s: " << last.datagramsRead
              << " datagrams read, " << last.beatsHeard << " beats of the followed device";
    if (last.cueBeatsDropped != 0) {
        std::cout << ", " << last.cueBeatsDropped << " cue presses' beats dropped";
    }
    if (last.sendsFailed != 0) {
        std::cout << ", " << last.sendsFailed << " sends failed";
    }
    std::cout << '\n' << std::flush;
    return 0;
}

} // namespace takt4::cli
