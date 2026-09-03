// takt4-cli — the development console (HANDOFF §8 Phase 1: list devices, open one
// channel, print RMS). Links takt4_core only; never packaged.

#include "core/audio/channel_meter.hpp"
#include "core/audio/channel_picker.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/hop_meter.hpp"
#include "core/audio/host_apis.hpp"
#include "core/audio/input_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/audio/resampler.hpp"
#include "core/build_info.hpp"
#include "core/rt/alloc_guard.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;

std::atomic<bool> g_interrupted{false};

void onSignal(int) {
    g_interrupted.store(true);
}

void printUsage(std::ostream& out) {
    out << "takt4-cli " << takt4::buildInfo().version << " — development console\n"
        << "\n"
        << "  takt4-cli devices\n"
        << "      List the host APIs, then every input device with its channels.\n"
        << "\n"
        << "  takt4-cli meter --device N (--channel C | --channels A,B | --all)\n"
        << "                  [--software] [--rate HZ] [--seconds S]\n"
        << "      Open device N (an index from `devices`) and print levels at 10 Hz.\n"
        << "      --channel C     one input, numbered from 1 as printed on the interface\n"
        << "      --channels A,B  two inputs summed to mono\n"
        << "      --all           every channel of the device, no resampling\n"
        << "      --software      slice channels in software even where the host API\n"
        << "                      could open the selection natively (ASIO, CoreAudio)\n"
        << "      --rate HZ       open at this rate instead of the device default\n"
        << "      --seconds S     stop after S seconds instead of on Ctrl-C\n"
        << "\n"
        << "  takt4-cli --version\n";
}

struct MeterArgs {
    int device = -1;
    std::optional<int> channel;                // 0-based
    std::optional<std::pair<int, int>> pair;   // 0-based
    bool all = false;
    bool software = false;
    double rate = 0.0;
    double seconds = 0.0; // 0: until interrupted
};

int parseInt(std::string_view text, std::string_view what) {
    std::size_t consumed = 0;
    const std::string s(text);
    int value = 0;
    try {
        value = std::stoi(s, &consumed);
    } catch (const std::exception&) {
        consumed = 0;
    }
    if (consumed != s.size()) {
        throw std::invalid_argument(std::string(what) + ": not a number: " + s);
    }
    return value;
}

double parseDouble(std::string_view text, std::string_view what) {
    std::size_t consumed = 0;
    const std::string s(text);
    double value = 0.0;
    try {
        value = std::stod(s, &consumed);
    } catch (const std::exception&) {
        consumed = 0;
    }
    if (consumed != s.size()) {
        throw std::invalid_argument(std::string(what) + ": not a number: " + s);
    }
    return value;
}

int channelNumber(std::string_view text, std::string_view what) {
    const int number = parseInt(text, what);
    if (number < 1) {
        throw std::invalid_argument(std::string(what) + ": channels are numbered from 1");
    }
    return number - 1;
}

MeterArgs parseMeterArgs(const std::vector<std::string_view>& args) {
    MeterArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&](std::string_view name) -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(name) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--device") {
            out.device = parseInt(value(arg), arg);
        } else if (arg == "--channel") {
            out.channel = channelNumber(value(arg), arg);
        } else if (arg == "--channels") {
            const std::string_view v = value(arg);
            const std::size_t comma = v.find(',');
            if (comma == std::string_view::npos) {
                throw std::invalid_argument("--channels expects two numbers, A,B");
            }
            out.pair = std::make_pair(channelNumber(v.substr(0, comma), arg),
                                      channelNumber(v.substr(comma + 1), arg));
        } else if (arg == "--all") {
            out.all = true;
        } else if (arg == "--software") {
            out.software = true;
        } else if (arg == "--rate") {
            out.rate = parseDouble(value(arg), arg);
            if (!(out.rate > 0.0)) {
                throw std::invalid_argument("--rate must be positive");
            }
        } else if (arg == "--seconds") {
            out.seconds = parseDouble(value(arg), arg);
            if (!(out.seconds > 0.0)) {
                throw std::invalid_argument("--seconds must be positive");
            }
        } else {
            throw std::invalid_argument("unknown option: " + std::string(arg));
        }
    }
    if (out.device < 0) {
        throw std::invalid_argument("meter needs --device N");
    }
    const int selections = (out.channel ? 1 : 0) + (out.pair ? 1 : 0) + (out.all ? 1 : 0);
    if (selections != 1) {
        throw std::invalid_argument("meter needs exactly one of --channel, --channels, --all");
    }
    return out;
}

const takt4::audio::InputDevice& findDevice(const std::vector<takt4::audio::InputDevice>& devices,
                                            int index) {
    for (const auto& device : devices) {
        if (device.index == index) {
            return device;
        }
    }
    throw std::invalid_argument("no input device with index " + std::to_string(index) +
                                "; see `takt4-cli devices`");
}

// One decimal, fixed. Formatting goes through strings so std::cout's own flags stay
// untouched (a stray std::fixed/setprecision would turn 44100 into 4e+04 later).
std::string fixed1(double value, int width = 0) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << std::setw(width) << value;
    return out.str();
}

std::string formatDb(float linear) {
    return fixed1(static_cast<double>(takt4::audio::toDbfs(linear)), 6);
}

int runDevices() {
    const takt4::audio::PortAudioSession session;
    // Which back ends this build carries and could bring up here; a machine with no
    // audio hardware (a CI runner) still shows them.
    std::cout << "host APIs:";
    for (const auto& api : takt4::audio::listHostApis()) {
        std::cout << "  " << api.name << " (" << api.deviceCount << (api.deviceCount == 1 ? " device)" : " devices)");
    }
    std::cout << '\n';
    const auto devices = takt4::audio::listInputDevices(session);
    if (devices.empty()) {
        std::cout << "no input devices\n";
        return 0;
    }
    for (const auto& device : devices) {
        std::cout << std::setw(3) << device.index << "  " << device.hostApiName << " / " << device.name
                  << "  (" << device.maxInputChannels << " in @ " << device.defaultSampleRate << " Hz"
                  << ", latency " << fixed1(device.defaultLowInputLatency * 1000.0) << "-"
                  << fixed1(device.defaultHighInputLatency * 1000.0) << " ms"
                  << (device.isDefaultInput ? ", default" : "")
                  << (device.isLoopback ? ", loopback" : "")
                  << (takt4::audio::hasNativeChannelSelection(device.hostApi) ? ", native channel selection"
                                                                              : "")
                  << ")\n";
        for (std::size_t c = 0; c < device.channelNames.size(); ++c) {
            std::cout << "       " << std::setw(2) << c + 1 << ": " << device.channelNames[c] << '\n';
        }
    }
    return 0;
}

bool shouldStop(std::chrono::steady_clock::time_point start, double seconds) {
    if (g_interrupted.load()) {
        return true;
    }
    if (seconds > 0.0) {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return elapsed >= seconds;
    }
    return false;
}

int runMeterAll(const takt4::audio::PortAudioSession& session, const takt4::audio::InputDevice& device,
                const MeterArgs& args) {
    takt4::audio::ChannelMeter meter(session, device, args.rate);
    std::cout << "metering all " << meter.channelCount() << " channels of " << device.name << " @ "
              << meter.sampleRate() << " Hz (software slice, no resampling)\n";
    meter.start();

    std::vector<takt4::audio::ChannelMeter::Level> levels(static_cast<std::size_t>(meter.channelCount()));
    const auto start = std::chrono::steady_clock::now();
    while (!shouldStop(start, args.seconds)) {
        std::this_thread::sleep_for(100ms);
        meter.read(levels);
        const auto counters = meter.counters();
        std::ostringstream line;
        line << fixed1(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 6)
             << "s";
        for (std::size_t c = 0; c < levels.size(); ++c) {
            line << "  " << c + 1 << ":" << formatDb(levels[c].rms);
        }
        line << "  overflows " << counters.inputOverflows;
        std::cout << line.str() << '\n' << std::flush;
    }
    meter.stop();
    return 0;
}

int runMeterPicked(const takt4::audio::PortAudioSession& session,
                   const takt4::audio::InputDevice& device, const MeterArgs& args) {
    const takt4::audio::ChannelSelection selection =
        args.channel ? takt4::audio::ChannelSelection::single(*args.channel)
                     : takt4::audio::ChannelSelection::pair(args.pair->first, args.pair->second);

    takt4::audio::HopMeter hopMeter;
    takt4::audio::InputStreamOptions options;
    options.sampleRate = args.rate;
    options.forceSoftwareSlice = args.software;
    takt4::audio::InputStream stream(session, device, selection, hopMeter, options);

    const auto& picker = stream.picker();
    std::cout << "device:    " << device.hostApiName << " / " << device.name << '\n'
              << "channels:  ";
    if (selection.count == 1) {
        std::cout << selection.channels[0] + 1;
    } else {
        std::cout << selection.channels[0] + 1 << " + " << selection.channels[1] + 1 << " summed";
    }
    std::cout << " (" << takt4::audio::toString(picker.mode()) << " pick, stream opens "
              << picker.streamChannelCount() << " of " << device.maxInputChannels << " channels)\n"
              << "rate:      " << stream.sampleRate() << " Hz";
    if (stream.reportedSampleRate() != stream.sampleRate()) {
        std::cout << " (host reports " << stream.reportedSampleRate() << " Hz)";
    }
    std::cout << " -> " << takt4::audio::kInternalSampleRate << " Hz, hop " << takt4::audio::kHopSize
              << " samples\n"
              << "latency:   " << fixed1(stream.inputLatencySeconds() * 1000.0) << " ms input buffer + "
              << fixed1(1000.0 * static_cast<double>(stream.resamplerDelayFrames()) / stream.sampleRate())
              << " ms resampler (" << stream.resamplerDelayFrames() << " frames, r8brain "
              << takt4::audio::Resampler::libraryVersion() << ")\n"
              << "rt guard:  " << (takt4::rt::allocationGuardEnabled() ? "on" : "off") << '\n';

    stream.start();

    const auto start = std::chrono::steady_clock::now();
    while (!shouldStop(start, args.seconds)) {
        std::this_thread::sleep_for(100ms);

        // Fold the hops since the last line into one reading.
        double sumSquares = 0.0;
        float peak = 0.0f;
        int hops = 0;
        takt4::audio::HopLevel level;
        std::uint64_t lastHop = 0;
        while (hopMeter.pop(level)) {
            sumSquares += static_cast<double>(level.rms) * static_cast<double>(level.rms);
            peak = std::max(peak, level.peak);
            lastHop = level.hopIndex;
            ++hops;
        }
        const float rms = hops > 0 ? static_cast<float>(std::sqrt(sumSquares / hops)) : 0.0f;

        const auto counters = stream.counters();
        std::ostringstream line;
        line << fixed1(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 6)
             << "s"
             << "  rms " << formatDb(rms) << " dBFS  peak " << formatDb(peak) << " dBFS"
             << "  hops " << hops << " (#" << lastHop << ")"
             << "  frames " << counters.framesIn
             << "  callbacks " << counters.callbacks
             << "  overflows " << counters.inputOverflows
             << "  dropped " << hopMeter.dropped();
        std::cout << line.str() << '\n' << std::flush;
    }
    stream.stop();

    const auto counters = stream.counters();
    std::cout << "stopped after " << counters.framesIn << " frames, " << counters.hopsOut << " hops, "
              << counters.inputOverflows << " input overflows";
    if (takt4::rt::allocationGuardEnabled()) {
        std::cout << ", " << takt4::rt::violationCount() << " rt violations";
    }
    std::cout << '\n';
    return 0;
}

int runMeter(const std::vector<std::string_view>& args) {
    const MeterArgs parsed = parseMeterArgs(args);
    const takt4::audio::PortAudioSession session;
    const auto devices = takt4::audio::listInputDevices(session);
    const auto& device = findDevice(devices, parsed.device);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (parsed.all) {
        return runMeterAll(session, device, parsed);
    }
    return runMeterPicked(session, device, parsed);
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printUsage(std::cout);
        return args.empty() ? 1 : 0;
    }
    try {
        if (args[0] == "--version") {
            std::cout << takt4::describe(takt4::buildInfo());
            return 0;
        }
        if (args[0] == "devices") {
            return runDevices();
        }
        if (args[0] == "meter") {
            return runMeter({args.begin() + 1, args.end()});
        }
        std::cerr << "takt4-cli: unknown command: " << args[0] << "\n\n";
        printUsage(std::cerr);
        return 1;
    } catch (const std::invalid_argument& e) {
        std::cerr << "takt4-cli: " << e.what() << '\n';
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "takt4-cli: " << e.what() << '\n';
        return 1;
    }
}
