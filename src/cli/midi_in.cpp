#include "cli/midi_in.hpp"

#include "core/midi/midi_input_hub.hpp"
#include "core/output/link_session.hpp"
#include "core/output/midi_ports.hpp"

#include "cli/args.hpp"
#include "cli/console.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace takt4::cli {

void printMidiInUsage(std::ostream& out) {
    out << "  takt4-cli midi-in [PORT] [--seconds S] [--out FILE]\n"
        << "      With no port, list the MIDI inputs. With one — part of its name, or its index\n"
        << "      — listen to it as takt4 does, every message stamped on Link's clock as it\n"
        << "      arrives, and say how steady the MIDI clock on it is: once a second while it\n"
        << "      runs, and over the whole run at the end. Start, Continue, Stop and Song\n"
        << "      Position are printed as they come.\n"
        << "      --out FILE      every message, one a line: microseconds on Link's clock, a\n"
        << "                      tab, then its bytes in hex\n"
        << "      --seconds S     stop after S seconds instead of on Ctrl-C\n"
        << "\n";
}

namespace {

using namespace std::chrono_literals;

struct MidiInArgs {
    std::optional<std::string> port;
    double seconds = 0.0;
    std::optional<std::string> out;
};

MidiInArgs parseMidiInArgs(const std::vector<std::string_view>& args) {
    MidiInArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--seconds") {
            out.seconds = parseDouble(value(), arg);
            if (!(out.seconds > 0.0)) {
                throw std::invalid_argument("--seconds expects a positive number");
            }
        } else if (arg == "--out") {
            out.out = std::string(value());
        } else if (arg.rfind("--", 0) == 0) {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        } else if (!out.port) {
            out.port = std::string(arg);
        } else {
            throw std::invalid_argument("midi-in listens to one port; \"" + std::string(arg) +
                                        "\" is a second");
        }
    }
    return out;
}

/// One message as it arrived. A clock's are one to three bytes, and nothing longer reaches a
/// listener that wants them but a note's or a controller's, which are three too.
struct Stamped {
    std::int64_t at = 0;
    std::array<unsigned char, 3> bytes{};
    std::size_t size = 0;
};

/// Keeps what arrives, for the console's loop to take.
class Recorder final : public midi::MidiInputListener {
public:
    Recorder() { waiting_.reserve(kMostWaiting); }
    void onMidi(std::span<const unsigned char> message, std::int64_t atMicros) noexcept override {
        Stamped stamped;
        stamped.at = atMicros;
        stamped.size = std::min(message.size(), stamped.bytes.size());
        std::copy_n(message.begin(), stamped.size, stamped.bytes.begin());
        const std::lock_guard<std::mutex> lock(mutex_);
        // Room was made for this many, so the driver's thread never waits on an allocation.
        if (waiting_.size() < kMostWaiting) {
            waiting_.push_back(stamped);
        } else {
            ++lost_;
        }
    }
    bool wantsClock() const noexcept override { return true; }
    void take(std::vector<Stamped>& out) {
        out.clear();
        const std::lock_guard<std::mutex> lock(mutex_);
        out.swap(waiting_);
        waiting_.reserve(kMostWaiting);
    }
    std::uint64_t lost() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return lost_;
    }

private:
    static constexpr std::size_t kMostWaiting = 1 << 16;
    mutable std::mutex mutex_;
    std::vector<Stamped> waiting_;
    std::uint64_t lost_ = 0;
};

/// How steady a run of clock ticks is: their spacing and its spread, and how far each lands from
/// the straight line through all of them — which is what a receiver fitting a tempo to the ticks
/// has to see through. Meaningful for a clock at one tempo.
struct TickStats {
    std::size_t ticks = 0;
    double meanGapMicros = 0.0;
    double bpm = 0.0;
    double gapSdMicros = 0.0;
    double gapWorstMicros = 0.0; ///< the gap furthest from the mean, by how far
    double lineRmsMicros = 0.0;
    double lineWorstMicros = 0.0;
};

TickStats measure(const std::vector<std::int64_t>& ticks) {
    TickStats stats;
    stats.ticks = ticks.size();
    if (ticks.size() < 3) {
        return stats;
    }
    const std::size_t gaps = ticks.size() - 1;
    stats.meanGapMicros =
        static_cast<double>(ticks.back() - ticks.front()) / static_cast<double>(gaps);
    stats.bpm = 60e6 / (stats.meanGapMicros * 24.0);
    double square = 0.0;
    for (std::size_t i = 1; i < ticks.size(); ++i) {
        const double off = static_cast<double>(ticks[i] - ticks[i - 1]) - stats.meanGapMicros;
        square += off * off;
        stats.gapWorstMicros = std::max(stats.gapWorstMicros, std::abs(off));
    }
    stats.gapSdMicros = std::sqrt(square / static_cast<double>(gaps));
    // The least-squares line through (tick number, moment), from the first tick so the numbers
    // stay small.
    const double n = static_cast<double>(ticks.size());
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const double x = static_cast<double>(i);
        const double y = static_cast<double>(ticks[i] - ticks.front());
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    const double intercept = (sy - slope * sx) / n;
    double residuals = 0.0;
    for (std::size_t i = 0; i < ticks.size(); ++i) {
        const double off = static_cast<double>(ticks[i] - ticks.front()) -
                           (intercept + slope * static_cast<double>(i));
        residuals += off * off;
        stats.lineWorstMicros = std::max(stats.lineWorstMicros, std::abs(off));
    }
    stats.lineRmsMicros = std::sqrt(residuals / n);
    return stats;
}

std::string fixed(double value, int places) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(places) << value;
    return text.str();
}

std::string describe(const TickStats& stats) {
    if (stats.ticks < 3) {
        return std::to_string(stats.ticks) + " ticks";
    }
    return std::to_string(stats.ticks) + " ticks, " + fixed(stats.bpm, 3) + " BPM, gap " +
           fixed(stats.meanGapMicros / 1000.0, 3) + " ms ± " +
           fixed(stats.gapSdMicros / 1000.0, 3) + " (worst " +
           fixed(stats.gapWorstMicros / 1000.0, 3) + "), off a straight line " +
           fixed(stats.lineRmsMicros / 1000.0, 3) + " ms rms, " +
           fixed(stats.lineWorstMicros / 1000.0, 3) + " worst";
}

const char* nameOf(unsigned char status) {
    switch (status) {
    case 0xFA:
        return "Start";
    case 0xFB:
        return "Continue";
    case 0xFC:
        return "Stop";
    case 0xF2:
        return "Song Position";
    case 0xF1:
        return "timecode quarter frame";
    default:
        return nullptr;
    }
}

int listInputs() {
    const std::vector<std::string> inputs = output::listMidiInputPorts();
    if (inputs.empty()) {
        std::cout << "no MIDI inputs on this machine (or no usable MIDI API)\n";
        return 0;
    }
    std::cout << "MIDI inputs:\n";
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        std::cout << "  " << i << ": " << inputs[i] << '\n';
    }
    return 0;
}

} // namespace

int runMidiIn(const std::vector<std::string_view>& args) {
    const MidiInArgs parsed = parseMidiInArgs(args);
    if (!parsed.port) {
        return listInputs();
    }
    // Link's clock, which is what everything takt4 follows is stamped on. The session is never
    // switched on: only its clock is read.
    output::LinkSession link(120.0);
    midi::MidiInputHub hub([&link] { return link.now().count(); });
    Recorder recorder;
    const std::string name = hub.listen(*parsed.port, recorder, "midi-in: ");
    // Off the hub before the recorder goes, however this is left.
    struct StopsListening {
        midi::MidiInputHub& hub;
        Recorder& recorder;
        ~StopsListening() { hub.stopListening(recorder); }
    } stopsListening{hub, recorder};

    std::ofstream out;
    if (parsed.out) {
        out.open(*parsed.out, std::ios::trunc);
        if (!out) {
            throw std::runtime_error(*parsed.out + ": cannot create");
        }
    }
    std::cout << "listening: " << name << " — every message stamped on Link's clock\n"
              << std::flush;
    const Interrupts interrupts;
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&start] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    std::vector<Stamped> taken;
    std::vector<std::int64_t> ticks;
    std::vector<std::int64_t> lastSecond;
    std::int64_t firstAt = 0;
    double reportedAt = 0.0;
    std::uint64_t others = 0;
    while (!Interrupts::requested() && !(parsed.seconds > 0.0 && elapsed() >= parsed.seconds)) {
        std::this_thread::sleep_for(100ms);
        recorder.take(taken);
        for (const Stamped& message : taken) {
            if (firstAt == 0) {
                firstAt = message.at;
            }
            if (out.is_open()) {
                out << message.at << '\t';
                for (std::size_t i = 0; i < message.size; ++i) {
                    out << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(message.bytes[i]) << std::dec;
                    out << (i + 1 < message.size ? " " : "");
                }
                out << '\n';
            }
            const unsigned char status = message.bytes[0];
            if (status == 0xF8) {
                ticks.push_back(message.at);
                lastSecond.push_back(message.at);
            } else if (const char* named = nameOf(status)) {
                std::cout << "  " << fixed(static_cast<double>(message.at - firstAt) / 1e6, 3)
                          << "s  " << named;
                if (status == 0xF2 && message.size == 3) {
                    std::cout << ' ' << (message.bytes[1] | (message.bytes[2] << 7))
                              << " sixteenths";
                }
                std::cout << '\n';
            } else {
                ++others;
            }
        }
        if (elapsed() - reportedAt >= 1.0) {
            reportedAt = elapsed();
            if (!lastSecond.empty()) {
                std::cout << "  " << fixed(reportedAt, 1) << "s  " << describe(measure(lastSecond))
                          << '\n';
            }
            lastSecond.clear();
            std::cout << std::flush;
        }
    }
    std::cout << "over the whole run: " << describe(measure(ticks));
    if (others != 0) {
        std::cout << "; " << others << " other messages";
    }
    if (recorder.lost() != 0) {
        std::cout << "; " << recorder.lost() << " lost (the console fell behind)";
    }
    std::cout << '\n' << std::flush;
    return 0;
}

} // namespace takt4::cli
