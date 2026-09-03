// takt4-cli — the development console (HANDOFF §8 Phase 1: list devices, open one
// channel, print RMS; Phase 2: run the feature front end over a file). Links
// takt4_core only; never packaged.

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
#include "core/engine/beat_engine.hpp"
#include "core/features/dimensions.hpp"
#include "core/features/feature_extractor.hpp"
#include "core/io/npy_file.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/activation_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/link_session.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/rt/alloc_guard.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
// timeapi.h must follow windows.h.
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
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
        << "  takt4-cli features IN.wav OUT.npy [--compare GOLDEN.npy]\n"
        << "      Run the feature front end over a mono " << takt4::audio::kInternalSampleRate
        << " Hz WAV and write the\n"
        << "      (frames x " << takt4::features::kFeatureDim
        << ") float32 matrix as numpy's .npy.\n"
        << "      --compare G.npy  also print the largest difference to a matrix that\n"
        << "                       tools/make_golden.py computed with madmom; exit 1 if it\n"
        << "                       exceeds the Phase 2 tolerance\n"
        << "\n"
        << "  takt4-cli beats (IN.wav | --device N (--channel C | --channels A,B))\n"
        << "                  [--weights SET|PATH] [--software] [--rate HZ] [--seconds S]\n"
        << "      Run the feature front end and the BeatNet+ model over a file or a live\n"
        << "      input and print P(beat), P(downbeat) and P(non-beat) as they come.\n"
        << "      --weights S     generic (default), generic-main, af-non-percussive, or a\n"
        << "                      path to a .bin from tools/convert_weights.py\n"
        << "      The other options are the meter's, and mean the same.\n"
        << "\n"
        << "  takt4-cli track (IN.wav | --device N (--channel C | --channels A,B))\n"
        << "                  [--weights SET|PATH] [--bpm LO-HI] [--latency MS]\n"
        << "                  [--confidence T] [--seed N] [--link] [--osc HOST:PORT]\n"
        << "                  [--osc-prefix /NAME] [--midi-clock PORT]\n"
        << "                  [--software] [--rate HZ] [--seconds S]\n"
        << "      The whole chain: features, model, particle filter, tempo state machine\n"
        << "      and the output transports. Prints a line per beat with the tempo, the\n"
        << "      bar position and the meter, and a status line while it waits.\n"
        << "      --bpm LO-HI     the octave-fold window, default 70-140; \"off\" leaves\n"
        << "                      the filter's own tempo alone, as an evaluation wants\n"
        << "      --latency MS    added to every beat's timestamp; negative fires early\n"
        << "      --confidence T  hold the last tempo below this, default 0.15\n"
        << "      --seed N        the particle filter's seed, default 1\n"
        << "      --out FILE      write every beat as <seconds> TAB <beat in bar> TAB\n"
        << "                      <BPM>; the first two columns are the format the\n"
        << "                      beat-tracking datasets annotate in\n"
        << "      --link          join the Ableton Link network as tempo master\n"
        << "      --osc H:P       send the generic namespace there; repeat for more\n"
        << "      --osc-prefix P  that namespace's prefix, default /takt4\n"
        << "      --midi-clock P  send 24 PPQN to the MIDI output port named P (a\n"
        << "                      substring of its name, or its index)\n"
        << "      The outputs need a live input; over a file only the beats are printed.\n"
        << "      The other options are the meter's and `beats`', and mean the same.\n"
        << "\n"
        << "  takt4-cli --version\n";
}

struct MeterArgs {
    int device = -1;
    std::optional<int> channel;              // 0-based
    std::optional<std::pair<int, int>> pair; // 0-based
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
        std::cout << "  " << api.name << " (" << api.deviceCount
                  << (api.deviceCount == 1 ? " device)" : " devices)");
    }
    std::cout << '\n';
    const auto devices = takt4::audio::listInputDevices(session);
    if (devices.empty()) {
        std::cout << "no input devices\n";
        return 0;
    }
    for (const auto& device : devices) {
        std::cout << std::setw(3) << device.index << "  " << device.hostApiName << " / "
                  << device.name << "  (" << device.maxInputChannels << " in @ "
                  << device.defaultSampleRate << " Hz"
                  << ", latency " << fixed1(device.defaultLowInputLatency * 1000.0) << "-"
                  << fixed1(device.defaultHighInputLatency * 1000.0) << " ms"
                  << (device.isDefaultInput ? ", default" : "")
                  << (device.isLoopback ? ", loopback" : "")
                  << (takt4::audio::hasNativeChannelSelection(device.hostApi)
                          ? ", native channel selection"
                          : "")
                  << ")\n";
        for (std::size_t c = 0; c < device.channelNames.size(); ++c) {
            std::cout << "       " << std::setw(2) << c + 1 << ": " << device.channelNames[c]
                      << '\n';
        }
    }
    return 0;
}

bool shouldStop(std::chrono::steady_clock::time_point start, double seconds) {
    if (g_interrupted.load()) {
        return true;
    }
    if (seconds > 0.0) {
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return elapsed >= seconds;
    }
    return false;
}

int runMeterAll(const takt4::audio::PortAudioSession& session,
                const takt4::audio::InputDevice& device, const MeterArgs& args) {
    takt4::audio::ChannelMeter meter(session, device, args.rate);
    std::cout << "metering all " << meter.channelCount() << " channels of " << device.name << " @ "
              << meter.sampleRate() << " Hz (software slice, no resampling)\n";
    meter.start();

    std::vector<takt4::audio::ChannelMeter::Level> levels(
        static_cast<std::size_t>(meter.channelCount()));
    const auto start = std::chrono::steady_clock::now();
    while (!shouldStop(start, args.seconds)) {
        std::this_thread::sleep_for(100ms);
        meter.read(levels);
        const auto counters = meter.counters();
        std::ostringstream line;
        line << fixed1(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    6)
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
    std::cout << " -> " << takt4::audio::kInternalSampleRate << " Hz, hop "
              << takt4::audio::kHopSize << " samples\n"
              << "latency:   " << fixed1(stream.inputLatencySeconds() * 1000.0)
              << " ms input buffer + "
              << fixed1(1000.0 * static_cast<double>(stream.resamplerDelayFrames()) /
                        stream.sampleRate())
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
        line << fixed1(
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    6)
             << "s"
             << "  rms " << formatDb(rms) << " dBFS  peak " << formatDb(peak) << " dBFS"
             << "  hops " << hops << " (#" << lastHop << ")"
             << "  frames " << counters.framesIn << "  callbacks " << counters.callbacks
             << "  overflows " << counters.inputOverflows << "  dropped " << hopMeter.dropped();
        std::cout << line.str() << '\n' << std::flush;
    }
    stream.stop();

    const auto counters = stream.counters();
    std::cout << "stopped after " << counters.framesIn << " frames, " << counters.hopsOut
              << " hops, " << counters.inputOverflows << " input overflows";
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

// The Phase 2 gate, by hand: the same hop-by-hop path the tests take (pad to whole
// hops, push, flush), written out for numpy, and optionally held against madmom's.
int runFeatures(const std::vector<std::string_view>& args) {
    std::vector<std::string_view> positional;
    std::optional<std::filesystem::path> golden;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--compare") {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("--compare needs a value");
            }
            golden = std::filesystem::path(args[++i]);
        } else if (args[i].starts_with("--")) {
            throw std::invalid_argument("unknown option: " + std::string(args[i]));
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2) {
        throw std::invalid_argument("features needs IN.wav and OUT.npy");
    }
    const std::filesystem::path in(positional[0]);
    const std::filesystem::path out(positional[1]);

    const takt4::io::WavData audio = takt4::io::readWavFile(in);
    if (audio.channels != 1) {
        throw std::invalid_argument(in.string() + ": expected mono, got " +
                                    std::to_string(audio.channels) +
                                    " channels (tools/make_golden.py writes what this reads)");
    }
    if (static_cast<double>(audio.sampleRate) != takt4::audio::kInternalSampleRate) {
        throw std::invalid_argument(
            in.string() + ": expected " +
            std::to_string(static_cast<int>(takt4::audio::kInternalSampleRate)) + " Hz, got " +
            std::to_string(audio.sampleRate));
    }

    using takt4::audio::kHopSize;
    using takt4::features::kFeatureDim;
    const std::size_t hops = (audio.samples.size() + kHopSize - 1) / kHopSize;
    std::vector<float> padded(hops * kHopSize, 0.0f);
    std::copy(audio.samples.begin(), audio.samples.end(), padded.begin());

    takt4::features::FeatureExtractor extractor;
    std::vector<float> matrix;
    matrix.reserve(hops * kFeatureDim);
    const auto collect = [&](bool delivered) {
        if (delivered) {
            matrix.insert(matrix.end(), extractor.frame().begin(), extractor.frame().end());
        }
    };
    for (std::size_t h = 0; h < hops; ++h) {
        collect(extractor.pushHop(
            std::span<const float, kHopSize>(padded.data() + h * kHopSize, kHopSize)));
    }
    collect(extractor.flush());
    const std::size_t frames = matrix.size() / kFeatureDim;

    takt4::io::writeNpyFloat32(out, frames, kFeatureDim, matrix);
    std::cout << in.string() << ": " << audio.samples.size() << " samples, " << hops << " hops -> "
              << frames << " frames x " << kFeatureDim << " written to " << out.string() << '\n';

    if (!golden) {
        return 0;
    }
    constexpr double kTolerance = 1e-5; // as tests/features/feature_extractor_test.cpp
    const takt4::io::NpyMatrix reference = takt4::io::readNpyFloat32(*golden);
    if (reference.rows != frames || reference.cols != kFeatureDim) {
        std::cout << golden->string() << ": shape (" << reference.rows << ", " << reference.cols
                  << ") does not match (" << frames << ", " << kFeatureDim << ")\n";
        return 1;
    }
    double worst = 0.0;
    std::size_t worstFrame = 0;
    std::size_t worstColumn = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t c = 0; c < kFeatureDim; ++c) {
            const double diff = std::abs(static_cast<double>(matrix[f * kFeatureDim + c]) -
                                         static_cast<double>(reference.at(f, c)));
            if (diff > worst) {
                worst = diff;
                worstFrame = f;
                worstColumn = c;
            }
        }
    }
    std::ostringstream line;
    line << std::scientific << std::setprecision(3) << "largest difference to " << golden->string()
         << ": " << worst << " at frame " << worstFrame << ", column " << worstColumn
         << " (tolerance " << kTolerance << "): " << (worst <= kTolerance ? "PASS" : "FAIL");
    std::cout << line.str() << '\n';
    return worst <= kTolerance ? 0 : 1;
}

// HANDOFF §8 Phase 3: the console prints the three class probabilities live.
struct BeatsArgs {
    std::optional<std::filesystem::path> file; // offline instead of a device
    MeterArgs stream;
    std::string weights = "generic";
};

BeatsArgs parseBeatsArgs(const std::vector<std::string_view>& args) {
    BeatsArgs out;
    std::vector<std::string_view> forwarded;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--weights") {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("--weights needs a value");
            }
            out.weights = std::string(args[++i]);
        } else if (args[i].starts_with("--")) {
            forwarded.push_back(args[i]);
            // Options that take a value carry it along.
            if ((args[i] == "--device" || args[i] == "--channel" || args[i] == "--channels" ||
                 args[i] == "--rate" || args[i] == "--seconds") &&
                i + 1 < args.size()) {
                forwarded.push_back(args[++i]);
            }
        } else if (!out.file) {
            out.file = std::filesystem::path(args[i]);
        } else {
            throw std::invalid_argument("beats takes at most one input file");
        }
    }
    if (out.file) {
        if (!forwarded.empty()) {
            throw std::invalid_argument("beats over a file takes no device options");
        }
        return out;
    }
    out.stream = parseMeterArgs(forwarded);
    if (out.stream.all) {
        throw std::invalid_argument("beats tracks one input, not --all");
    }
    return out;
}

// A bare name is one of the committed sets; anything with a separator or a suffix is
// taken as a path, so a set converted somewhere else can be tried without a rebuild.
std::filesystem::path resolveWeights(const std::string& spec) {
    const std::filesystem::path given(spec);
    if (given.has_parent_path() || given.has_extension()) {
        return given;
    }
    return std::filesystem::path(TAKT4_WEIGHTS_DIR) / (spec + ".bin");
}

std::string formatProbability(float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << std::setw(5) << static_cast<double>(value);
    return out.str();
}

// One line per frame, with a bar so a run is readable as it scrolls past.
std::string activationLine(const takt4::model::FrameActivation& activation) {
    constexpr int kBarWidth = 24;
    const auto bar = [&](float value) {
        const int filled = std::clamp(static_cast<int>(value * kBarWidth + 0.5f), 0, kBarWidth);
        return std::string(static_cast<std::size_t>(filled), '#') +
               std::string(static_cast<std::size_t>(kBarWidth - filled), '.');
    };
    std::ostringstream line;
    line << std::setw(7) << activation.frameIndex << "  "
         << fixed1(static_cast<double>(activation.frameIndex) / takt4::audio::kHopRate, 7) << "s"
         << "  beat " << formatProbability(activation.beat) << " " << bar(activation.beat)
         << "  down " << formatProbability(activation.downbeat) << "  non "
         << formatProbability(activation.nonBeat);
    return line.str();
}

int runBeatsFile(const std::filesystem::path& in, const takt4::model::ModelWeights& weights) {
    const takt4::io::WavData audio = takt4::io::readWavFile(in);
    if (audio.channels != 1 ||
        static_cast<double>(audio.sampleRate) != takt4::audio::kInternalSampleRate) {
        throw std::invalid_argument(
            in.string() + ": expected mono at " +
            std::to_string(static_cast<int>(takt4::audio::kInternalSampleRate)) + " Hz");
    }
    using takt4::audio::kHopSize;
    const std::size_t hops = (audio.samples.size() + kHopSize - 1) / kHopSize;
    std::vector<float> padded(hops * kHopSize, 0.0f);
    std::copy(audio.samples.begin(), audio.samples.end(), padded.begin());

    // The engine's own path, stepped on this thread: what the worker would compute.
    auto engine = std::make_unique<takt4::model::ActivationEngine>(weights);
    std::cout << in.string() << ": " << hops << " hops, weights "
              << weights.path().filename().string() << '\n';
    takt4::model::FrameActivation activation;
    double loudest = 0.0;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(padded.data() + h * kHopSize, h);
        (void)engine->step();
        while (engine->pop(activation)) {
            std::cout << activationLine(activation) << '\n';
            loudest = std::max(loudest, static_cast<double>(activation.beat));
        }
    }
    std::cout << engine->framesEmitted() << " frames, strongest beat probability "
              << fixed1(loudest * 100.0) << "%, hop " << fixed1(engine->meanHopMicros())
              << " us mean / " << fixed1(engine->worstHopMicros())
              << " us worst, of 20000 us of audio\n";
    return 0;
}

int runBeatsDevice(const BeatsArgs& parsed, const takt4::model::ModelWeights& weights) {
    const takt4::audio::PortAudioSession session;
    const auto devices = takt4::audio::listInputDevices(session);
    const auto& device = findDevice(devices, parsed.stream.device);
    const takt4::audio::ChannelSelection selection =
        parsed.stream.channel ? takt4::audio::ChannelSelection::single(*parsed.stream.channel)
                              : takt4::audio::ChannelSelection::pair(parsed.stream.pair->first,
                                                                     parsed.stream.pair->second);

    // `beats` is the Phase 3 command and stops at the network's output, so it drives the
    // activation engine directly rather than through the tracker.
    auto engine = std::make_unique<takt4::model::ActivationEngine>(weights);
    takt4::audio::InputStreamOptions options;
    options.sampleRate = parsed.stream.rate;
    options.forceSoftwareSlice = parsed.stream.software;
    takt4::audio::InputStream stream(session, device, selection, *engine, options);

    std::cout << "device:    " << device.hostApiName << " / " << device.name << '\n'
              << "channel:   " << selection.channels[0] + 1;
    if (selection.count == 2) {
        std::cout << " + " << selection.channels[1] + 1 << " summed";
    }
    std::cout << " (" << takt4::audio::toString(stream.picker().mode()) << " pick)\n"
              << "rate:      " << stream.sampleRate() << " Hz -> "
              << takt4::audio::kInternalSampleRate << " Hz, hop " << takt4::audio::kHopSize
              << " samples\n"
              << "weights:   " << weights.path().string() << '\n'
              << "latency:   " << fixed1(stream.inputLatencySeconds() * 1000.0)
              << " ms input buffer + "
              << fixed1(1000.0 * static_cast<double>(stream.resamplerDelayFrames()) /
                        stream.sampleRate())
              << " ms resampler + 40.0 ms centred framing\n"
              << "rt guard:  " << (takt4::rt::allocationGuardEnabled() ? "on" : "off") << '\n';

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    engine->start();
    stream.start();

    const auto start = std::chrono::steady_clock::now();
    takt4::model::FrameActivation activation;
    while (!shouldStop(start, parsed.stream.seconds)) {
        std::this_thread::sleep_for(20ms);
        while (engine->pop(activation)) {
            std::cout << activationLine(activation) << '\n';
        }
        std::cout << std::flush;
    }
    stream.stop();
    engine->stop();
    while (engine->pop(activation)) {
        std::cout << activationLine(activation) << '\n';
    }

    const auto counters = stream.counters();
    std::cout << "stopped after " << counters.hopsOut << " hops, " << engine->framesEmitted()
              << " frames"
              << ", " << engine->hopsDropped() << " hops dropped"
              << ", " << engine->framesDropped() << " frames dropped"
              << ", " << counters.inputOverflows << " input overflows\n"
              << "worst hop " << fixed1(engine->worstHopMicros()) << " us (model alone "
              << fixed1(engine->worstModelMicros()) << " us) of 20000 us of audio\n";
    return 0;
}

// HANDOFF §8 Phase 4: the console binary tracks tempo, downbeat and meter, and drives
// Link and OSC.
struct TrackArgs {
    BeatsArgs beats;
    takt4::tracking::TempoTracker::Options tempo;
    std::uint64_t seed = 1;
    bool link = false;
    std::optional<std::filesystem::path> beatsOut;
    std::string oscPrefix = "/takt4";
    std::vector<std::pair<std::string, std::uint16_t>> oscTargets;
    std::optional<std::string> midiClockPort;

    bool anyOutput() const { return link || !oscTargets.empty() || midiClockPort.has_value(); }
};

std::pair<double, double> parseRange(std::string_view text, std::string_view what) {
    const std::size_t dash = text.find('-', 1);
    if (dash == std::string_view::npos) {
        throw std::invalid_argument(std::string(what) + " expects LO-HI");
    }
    const double low = parseDouble(text.substr(0, dash), what);
    const double high = parseDouble(text.substr(dash + 1), what);
    if (!(low > 0.0) || !(high > low)) {
        throw std::invalid_argument(std::string(what) +
                                    ": LO-HI must be an ascending positive range");
    }
    return {low, high};
}

TrackArgs parseTrackArgs(const std::vector<std::string_view>& args) {
    TrackArgs out;
    std::vector<std::string_view> forwarded;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--bpm") {
            const std::string_view range = value();
            if (range == "off") {
                // What the published beat trackers are measured without; see
                // tools/evaluate.py.
                out.tempo.octaveFold = false;
            } else {
                const auto parsed = parseRange(range, arg);
                out.tempo.minBpm = parsed.first;
                out.tempo.maxBpm = parsed.second;
            }
        } else if (arg == "--latency") {
            out.tempo.latencyOffsetSeconds = parseDouble(value(), arg) / 1000.0;
        } else if (arg == "--confidence") {
            out.tempo.confidenceThreshold = parseDouble(value(), arg);
        } else if (arg == "--seed") {
            out.seed = static_cast<std::uint64_t>(parseInt(value(), arg));
        } else if (arg == "--link") {
            out.link = true;
        } else if (arg == "--out") {
            out.beatsOut = std::filesystem::path(value());
        } else if (arg == "--osc") {
            const std::string_view target = value();
            // The last colon separates the port, so a bare IPv6 address is still usable.
            const std::size_t colon = target.rfind(':');
            if (colon == std::string_view::npos || colon == 0) {
                throw std::invalid_argument("--osc expects HOST:PORT");
            }
            const int port = parseInt(target.substr(colon + 1), arg);
            if (port < 1 || port > 65535) {
                throw std::invalid_argument("--osc: the port must be 1 to 65535");
            }
            out.oscTargets.emplace_back(std::string(target.substr(0, colon)),
                                        static_cast<std::uint16_t>(port));
        } else if (arg == "--osc-prefix") {
            out.oscPrefix = std::string(value());
        } else if (arg == "--midi-clock") {
            out.midiClockPort = std::string(value());
        } else {
            forwarded.push_back(arg);
            if ((arg == "--device" || arg == "--channel" || arg == "--channels" ||
                 arg == "--rate" || arg == "--seconds" || arg == "--weights") &&
                i + 1 < args.size()) {
                forwarded.push_back(args[++i]);
            }
        }
    }
    out.beats = parseBeatsArgs(forwarded);
    return out;
}

std::filesystem::path stateSpacePath() {
    return std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin";
}

std::string beatLine(const takt4::tracking::BeatEvent& event,
                     const takt4::tracking::TempoState& state) {
    std::ostringstream line;
    line << fixed1(event.time, 8) << "s  " << (event.downbeat ? "DOWNBEAT" : "beat    ") << "  ";
    if (event.beatsPerBar > 0 && event.beatInBar > 0) {
        for (std::uint32_t b = 1; b <= event.beatsPerBar; ++b) {
            line << (b == event.beatInBar ? '#' : '.');
        }
        line << "  " << event.beatInBar << "/" << event.beatsPerBar;
    } else {
        line << "?    ";
    }
    std::ostringstream bpm;
    bpm << std::fixed << std::setprecision(2) << std::setw(7) << event.bpm;
    line << "  " << bpm.str() << " BPM  " << (event.locked ? "LOCKED  " : "hunting ") << "conf "
         << formatProbability(static_cast<float>(event.confidence));
    if (state.holding) {
        line << "  (holding)";
    }
    return line.str();
}

/// The transports, and the printing. The tracking is engine::BeatEngine's now.
///
/// Everything a transport is given comes from one beat: Link gets the host time the
/// audio actually arrived at (from the frame's stamp, HANDOFF §4.3), OSC gets the tempo
/// and the bar position, and the MIDI clock is re-phased on the beat and left to tick in
/// between. §4.2 puts this on an output thread of its own; the console drives it from
/// its drain loop instead, which is a deviation the handoff records and Phase 5's
/// restructuring is where it goes away.
class Transports {
public:
    explicit Transports(const TrackArgs& args)
        : latencyMicros_(static_cast<std::int64_t>(args.tempo.latencyOffsetSeconds * 1e6)) {
        if (!args.oscTargets.empty()) {
            osc_ = std::make_unique<takt4::output::OscPublisher>(args.oscPrefix);
            for (const auto& [host, port] : args.oscTargets) {
                osc_->addTarget(host, port);
            }
        }
        if (args.midiClockPort) {
            midiPort_ = std::make_unique<takt4::output::MidiOutput>(*args.midiClockPort);
            midi_ = std::make_unique<takt4::output::MidiClock>(*midiPort_, 120.0);
        }
        if (args.link) {
            link_ = std::make_unique<takt4::output::LinkSession>(120.0);
        }
    }

    takt4::output::LinkSession* link() const { return link_.get(); }
    takt4::output::OscPublisher* osc() const { return osc_.get(); }
    takt4::output::MidiClock* midiClock() const { return midi_.get(); }
    const takt4::output::MidiOutput* midiPort() const { return midiPort_.get(); }

    /// Starts the transports. `now` is the seconds-since-start clock the MIDI clock is
    /// ticked with; see advance().
    void startOutputs(double now) {
        if (link_) {
            link_->enable(true);
        }
        if (midi_) {
            midi_->start(now);
        }
    }

    void stopOutputs() {
        if (midi_) {
            midi_->stop();
        }
        if (link_) {
            link_->enable(false);
        }
    }

    /// Ticks the MIDI clock up to `now`, and republishes any OSC state that moved.
    void advance(double now, const takt4::tracking::TempoState& state) {
        if (midi_) {
            (void)midi_->advance(now);
        }
        if (osc_) {
            osc_->publishState(state);
        }
    }

    /// Writes every beat as "<seconds>\t<beat in bar>\t<BPM>". The first two columns are
    /// the format the beat-tracking datasets annotate in, so an estimate and a reference
    /// are the same kind of file and the same reader handles both; the third is what the
    /// tempo state machine was publishing at that beat, which is the only thing §5.5's
    /// octave fold can move. A fold cannot change a beat time, so it cannot show up in
    /// beat F-measure — the tempo column is where its effect is visible at all.
    void writeBeatsTo(const std::filesystem::path& path) {
        beatsOut_.open(path, std::ios::trunc);
        if (!beatsOut_) {
            throw std::runtime_error(path.string() + ": cannot create");
        }
        beatsOut_ << std::fixed << std::setprecision(6);
    }

    /// One beat: printed, written, and handed to whichever transports are on.
    /// `hostMicros` is the frame's §4.3 stamp, zero offline; `now` is the
    /// seconds-since-start clock the MIDI clock ticks on.
    void publish(const takt4::tracking::BeatEvent& event, const takt4::tracking::TempoState& state,
                 std::int64_t hostMicros, double now) {
        ++beats_;
        if (event.downbeat) {
            ++downbeats_;
        }
        std::cout << beatLine(event, state) << '\n';
        if (beatsOut_.is_open()) {
            beatsOut_ << event.time << '\t' << event.beatInBar << '\t' << event.bpm << '\n';
        }

        if (osc_) {
            osc_->publishBeat(event);
        }
        if (midi_) {
            midi_->setTempo(event.bpm);
            // The beat's audio arrived a pipeline's worth of time ago; the latency
            // offset is the one place that is compensated (§5.5).
            midi_->syncToBeat(now + static_cast<double>(latencyMicros_) / 1e6);
        }
        if (link_) {
            publishToLink(event, hostMicros);
        }
    }

    std::uint64_t beats() const { return beats_; }
    std::uint64_t downbeats() const { return downbeats_; }

private:
    void publishToLink(const takt4::tracking::BeatEvent& event, std::int64_t hostMicros) {
        // Without a host time source — the offline path — there is nothing meaningful to
        // align to, so Link is left alone.
        if (hostMicros == 0) {
            return;
        }
        const std::chrono::microseconds at{hostMicros + latencyMicros_};
        if (std::abs(event.bpm - lastLinkBpm_) > 0.005) {
            link_->setTempo(event.bpm, at);
            lastLinkBpm_ = event.bpm;
        }
        // §5.6: phase through requestBeatAtTime with the detected meter as the quantum.
        // The beat number is the bar position, so peers line up on our downbeat; before
        // the first downbeat the bar phase is unknown and only the tempo is published.
        if (event.beatInBar > 0 && event.beatsPerBar > 0) {
            link_->requestBeat(static_cast<double>(event.beatInBar - 1), at,
                               static_cast<double>(event.beatsPerBar));
        }
    }

    std::int64_t latencyMicros_ = 0;
    std::unique_ptr<takt4::output::LinkSession> link_;
    std::unique_ptr<takt4::output::OscPublisher> osc_;
    std::unique_ptr<takt4::output::MidiOutput> midiPort_;
    std::unique_ptr<takt4::output::MidiClock> midi_;
    std::ofstream beatsOut_;
    double lastLinkBpm_ = -1.0;
    std::uint64_t beats_ = 0;
    std::uint64_t downbeats_ = 0;
};

/// The engine's options from the console's arguments.
takt4::engine::BeatEngine::Options engineOptions(const TrackArgs& args) {
    takt4::engine::BeatEngine::Options options;
    options.filter.seed = args.seed;
    options.tempo = args.tempo;
    return options;
}

/// Drains one round of the engine and returns how many frames it had tracked.
///
/// The beats carry everything the transports need, including the state as it stood at
/// each one, so no pairing against the frame ring is needed — which is the point of
/// EngineBeat. The frames themselves are drained and dropped: the console has no use for
/// the per-frame trace, which is §5.9's scrolling activation display and the UI's
/// business, but a ring nobody drains fills up and the engine would rightly start
/// counting frames lost.
std::size_t drain(takt4::engine::BeatEngine& engine, Transports& transports, double now) {
    takt4::engine::EngineFrame frame;
    std::size_t frames = 0;
    while (engine.popFrame(frame)) {
        ++frames;
    }
    takt4::engine::EngineBeat beat;
    while (engine.popBeat(beat)) {
        transports.publish(beat.event, beat.state, beat.hostMicros, now);
    }
    transports.advance(now, engine.state());
    return frames;
}

int runTrackFile(const std::filesystem::path& in, const takt4::model::ModelWeights& weights,
                 const takt4::tracking::StateSpaceModel& model, const TrackArgs& args) {
    const takt4::io::WavData audio = takt4::io::readWavFile(in);
    if (audio.channels != 1 ||
        static_cast<double>(audio.sampleRate) != takt4::audio::kInternalSampleRate) {
        throw std::invalid_argument(
            in.string() + ": expected mono at " +
            std::to_string(static_cast<int>(takt4::audio::kInternalSampleRate)) + " Hz");
    }
    using takt4::audio::kHopSize;
    const std::size_t hops = (audio.samples.size() + kHopSize - 1) / kHopSize;
    std::vector<float> padded(hops * kHopSize, 0.0f);
    std::copy(audio.samples.begin(), audio.samples.end(), padded.begin());

    // A file is worked through as fast as it reads, so its beats do not happen in real
    // time and there is no host clock to align a transport to. The outputs are left out
    // rather than driven with a timeline that never existed.
    TrackArgs offline = args;
    offline.link = false;
    offline.oscTargets.clear();
    offline.midiClockPort.reset();

    auto engine = std::make_unique<takt4::engine::BeatEngine>(weights, model, engineOptions(args));
    Transports transports(offline);
    if (args.beatsOut) {
        transports.writeBeatsTo(*args.beatsOut);
    }
    std::cout << in.string() << ": " << hops << " hops, weights "
              << weights.path().filename().string() << ", fold " << fixed1(args.tempo.minBpm) << "-"
              << fixed1(args.tempo.maxBpm) << " BPM, seed " << args.seed << '\n';
    if (args.anyOutput()) {
        std::cout << "note: Link, OSC and MIDI clock are driven from a live input only; "
                     "over a file this prints beats and nothing else.\n";
    }

    std::size_t frames = 0;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(padded.data() + h * kHopSize, h);
        (void)engine->step();
        // Offline the "now" a transport would tick on is the audio's own time; nothing is
        // enabled here, but passing anything else would be a lie in the argument.
        frames += drain(*engine, transports,
                        static_cast<double>(engine->framesTracked()) * model.secondsPerFrame());
    }
    const takt4::tracking::TempoState state = engine->state();
    std::cout << frames << " frames, " << transports.beats() << " beats (" << transports.downbeats()
              << " downbeats), ending at " << fixed1(state.bpm) << " BPM in " << state.beatsPerBar
              << "/4, " << (state.locked ? "locked" : "not locked") << '\n';
    return 0;
}

int runTrackDevice(const TrackArgs& args, const takt4::model::ModelWeights& weights,
                   const takt4::tracking::StateSpaceModel& model) {
    const takt4::audio::PortAudioSession session;
    const auto devices = takt4::audio::listInputDevices(session);
    const auto& device = findDevice(devices, args.beats.stream.device);
    const takt4::audio::ChannelSelection selection =
        args.beats.stream.channel
            ? takt4::audio::ChannelSelection::single(*args.beats.stream.channel)
            : takt4::audio::ChannelSelection::pair(args.beats.stream.pair->first,
                                                   args.beats.stream.pair->second);

    auto engine = std::make_unique<takt4::engine::BeatEngine>(weights, model, engineOptions(args));
    takt4::audio::InputStreamOptions options;
    options.sampleRate = args.beats.stream.rate;
    options.forceSoftwareSlice = args.beats.stream.software;
    takt4::audio::InputStream stream(session, device, selection, *engine, options);
    Transports transports(args);
    if (args.beatsOut) {
        transports.writeBeatsTo(*args.beatsOut);
    }
    // HANDOFF §4.3: the audio thread stamps each hop through Link's regression, so a
    // beat carries the host time of the audio it was found in rather than of the moment
    // this loop happened to notice it.
    if (transports.link() != nullptr) {
        engine->setHostTimeSource(transports.link());
    }

    std::cout << "device:    " << device.hostApiName << " / " << device.name << '\n'
              << "channel:   " << selection.channels[0] + 1;
    if (selection.count == 2) {
        std::cout << " + " << selection.channels[1] + 1 << " summed";
    }
    std::cout << " (" << takt4::audio::toString(stream.picker().mode()) << " pick)\n"
              << "weights:   " << weights.path().filename().string() << '\n'
              << "tracker:   " << model.path().filename().string() << ", fold "
              << fixed1(args.tempo.minBpm) << "-" << fixed1(args.tempo.maxBpm)
              << " BPM, confidence gate " << fixed1(args.tempo.confidenceThreshold * 100.0)
              << "%, latency offset " << fixed1(args.tempo.latencyOffsetSeconds * 1000.0) << " ms\n"
              << "latency:   " << fixed1(stream.inputLatencySeconds() * 1000.0)
              << " ms input buffer + "
              << fixed1(1000.0 * static_cast<double>(stream.resamplerDelayFrames()) /
                        stream.sampleRate())
              << " ms resampler + 40.0 ms centred framing\n";
    std::cout << "outputs:   ";
    if (!args.anyOutput()) {
        std::cout << "none (--link, --osc HOST:PORT, --midi-clock PORT)";
    }
    if (transports.link() != nullptr) {
        std::cout << "Link ";
    }
    if (transports.osc() != nullptr) {
        for (std::size_t i = 0; i < transports.osc()->targetCount(); ++i) {
            std::cout << "OSC " << transports.osc()->target(i).resolved() << " ";
        }
    }
    if (transports.midiPort() != nullptr) {
        std::cout << "MIDI clock to \"" << transports.midiPort()->portName() << "\"";
    }
    std::cout << '\n';

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    engine->start();
    stream.start();

    const auto start = std::chrono::steady_clock::now();
    transports.startOutputs(0.0);
    auto lastStatus = start;
    // A MIDI clock tick is 19 ms apart at 130 BPM, so the loop has to come round faster
    // than that or the ticks inherit its period as jitter. Without an output there is
    // nothing to be punctual for. Note that the tracking no longer waits on this loop:
    // the engine's inference thread runs at the audio's pace, and this only decides how
    // promptly a beat it already found reaches a transport.
    const auto period = args.anyOutput() ? 1ms : 10ms;
    while (!shouldStop(start, args.beats.stream.seconds)) {
        std::this_thread::sleep_for(period);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        (void)drain(*engine, transports, elapsed);
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatus >= 2s) {
            lastStatus = now;
            const takt4::tracking::TempoState state = engine->state();
            std::cout << "         "
                      << fixed1(std::chrono::duration<double>(now - start).count(), 8)
                      << "s  ..        " << fixed1(state.bpm, 12) << " BPM  "
                      << (state.locked ? "LOCKED  " : "hunting ") << "conf "
                      << formatProbability(static_cast<float>(state.confidence))
                      << (state.holding ? "  (holding)" : "") << '\n';
        }
        std::cout << std::flush;
    }
    stream.stop();
    engine->stop(); // tracks whatever the model worker had left before joining
    engine->setHostTimeSource(nullptr);
    (void)drain(*engine, transports,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    transports.stopOutputs();

    const auto counters = stream.counters();
    std::cout << "stopped after " << counters.hopsOut << " hops, " << engine->framesTracked()
              << " frames, " << transports.beats() << " beats (" << transports.downbeats()
              << " downbeats), " << counters.inputOverflows << " input overflows, "
              << engine->activations().hopsDropped() << " hops dropped\n"
              << "inference: " << fixed1(engine->meanFrameMicros()) << " us mean / "
              << fixed1(engine->worstFrameMicros()) << " us worst per frame, of 20000 us of audio"
              << (engine->framesDropped() != 0
                      ? ", " + std::to_string(engine->framesDropped()) + " frames not drained"
                      : "")
              << '\n';
    if (transports.osc() != nullptr) {
        std::cout << "OSC: " << transports.osc()->messagesSent() << " messages sent, "
                  << transports.osc()->messagesFailed() << " failed\n";
    }
    if (transports.midiClock() != nullptr) {
        std::cout << "MIDI clock: " << transports.midiClock()->ticksSent() << " ticks, "
                  << transports.midiClock()->ticksSkipped() << " skipped\n";
    }
    if (transports.link() != nullptr) {
        std::cout << "Link: " << transports.link()->tempoUpdates() << " tempo updates, "
                  << transports.link()->beatRequests() << " beat requests, "
                  << transports.link()->numPeers() << " peers at the end\n";
    }
    return 0;
}

int runTrack(const std::vector<std::string_view>& args) {
    const TrackArgs parsed = parseTrackArgs(args);
    const takt4::model::ModelWeights weights =
        takt4::model::ModelWeights::fromFile(resolveWeights(parsed.beats.weights));
    const takt4::tracking::StateSpaceModel model =
        takt4::tracking::StateSpaceModel::fromFile(stateSpacePath());
    if (parsed.beats.file) {
        return runTrackFile(*parsed.beats.file, weights, model, parsed);
    }
    return runTrackDevice(parsed, weights, model);
}

int runBeats(const std::vector<std::string_view>& args) {
    const BeatsArgs parsed = parseBeatsArgs(args);
    const takt4::model::ModelWeights weights =
        takt4::model::ModelWeights::fromFile(resolveWeights(parsed.weights));
    if (parsed.file) {
        return runBeatsFile(*parsed.file, weights);
    }
    return runBeatsDevice(parsed, weights);
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
        if (args[0] == "features") {
            return runFeatures({args.begin() + 1, args.end()});
        }
        if (args[0] == "beats") {
            return runBeats({args.begin() + 1, args.end()});
        }
        if (args[0] == "track") {
#if defined(_WIN32)
            // The tracking loop wakes every millisecond to keep the MIDI clock's ticks
            // punctual, and Windows' default timer granularity is 15.6 ms. Raising it is
            // process-wide and reverted on the way out.
            const bool raised = ::timeBeginPeriod(1) == TIMERR_NOERROR;
            const int result = runTrack({args.begin() + 1, args.end()});
            if (raised) {
                ::timeEndPeriod(1);
            }
            return result;
#else
            return runTrack({args.begin() + 1, args.end()});
#endif
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
