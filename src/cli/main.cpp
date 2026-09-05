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
#include "core/engine/control.hpp"
#include "core/features/dimensions.hpp"
#include "core/features/feature_extractor.hpp"
#include "core/io/npy_file.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/activation_engine.hpp"
#include "core/model/weights.hpp"
#include "core/output/link_session.hpp"
#include "core/output/midi_clock.hpp"
#include "core/output/osc_publisher.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/rt/alloc_guard.hpp"
#include "core/tracking/particle_filter.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tap_tempo.hpp"
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
#include <mutex>
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
#include <conio.h>
#include <windows.h> // GetConsoleMode for the key reader, SetConsoleOutputCP for UTF-8
#else
#include <termios.h>
#include <unistd.h>
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
        << "      On a live input the manual controls are on the keyboard: space taps the\n"
        << "      tempo, d snaps the downbeat to the next beat, h and x halve and double\n"
        << "      it, [ and ] move the latency offset, f turns the octave fold off and\n"
        << "      on, q stops. These need a console: MinTTY, which Git Bash uses, is not\n"
        << "      one, and the banner says so when the keys are unavailable.\n"
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

/// Single keys from the terminal, without waiting for one.
///
/// §5.5's manual controls — tap, the octave shift, the downbeat snap, the latency slider
/// — need somewhere to be pressed before there is a UI, and `BeatEngine`'s control queue
/// needs a producer that is not a test. This is both, and it is the shape a UI's input
/// handling takes too: read a key, post a command, never touch the tracker.
///
/// It does nothing at all unless stdin is something keys can actually come from. CI runs
/// this binary without a console, and a redirected stdin must never be left in a mode
/// nobody puts back.
class KeyReader {
public:
    KeyReader() {
#if defined(_WIN32)
        // Not `_isatty`: that is true of any character device, and NUL is one — measured,
        // a run with stdin redirected from /dev/null was called interactive. Only a real
        // console input handle has a console mode, and a console input handle is the only
        // thing _kbhit reads from anyway.
        DWORD mode = 0;
        const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        interactive_ =
            in != nullptr && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode) != 0;
#else
        interactive_ = isatty(STDIN_FILENO) != 0;
        if (!interactive_) {
            return;
        }
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) {
            interactive_ = false;
            return;
        }
        struct termios raw = saved_;
        // Keys as they are pressed rather than lines, and not echoed back in among the
        // beats. ISIG is left alone, so Ctrl+C still reaches the signal handler.
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        // VMIN 0 with VTIME 0 is what makes read() return at once with whatever is there,
        // rather than blocking the loop that has a MIDI clock to tick.
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            interactive_ = false;
            return;
        }
        restore_ = true;
#endif
    }

    ~KeyReader() {
#if !defined(_WIN32)
        if (restore_) {
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
        }
#endif
    }

    KeyReader(const KeyReader&) = delete;
    KeyReader& operator=(const KeyReader&) = delete;

    bool interactive() const noexcept { return interactive_; }

    /// The next key waiting, or 0 when none is.
    int poll() noexcept {
        if (!interactive_) {
            return 0;
        }
#if defined(_WIN32)
        if (_kbhit() == 0) {
            return 0;
        }
        const int key = _getch();
        // An arrow or function key arrives as a 0 or 0xE0 prefix and then its scan code.
        // Both bytes are already buffered; leaving the second would hand back a scan code
        // that collides with a letter on the next poll.
        if (key == 0 || key == 0xE0) {
            (void)_getch();
            return 0;
        }
        return key;
#else
        char pressed = 0;
        return read(STDIN_FILENO, &pressed, 1) == 1 ? static_cast<unsigned char>(pressed) : 0;
#endif
    }

private:
    bool interactive_ = false;
#if !defined(_WIN32)
    struct termios saved_{};
    bool restore_ = false;
#endif
};

void printKeys(std::ostream& out, bool available) {
    if (!available) {
        // Worth saying rather than silently doing nothing. On Windows this is most often
        // a terminal that is not a console: MinTTY, which Git Bash uses, connects stdin
        // as a pipe, and _kbhit only ever reads a console input buffer. cmd, PowerShell
        // and Windows Terminal all give a real one.
        out << "keys:      not available — stdin is not a console\n";
        return;
    }
    out << "keys:      space tap tempo · d downbeat now · h /2 · x *2\n"
        << "           [ ] latency offset -/+ 5 ms · f octave fold on/off · q quit\n";
}

/// The console's half of a beat: the line it prints and the file it optionally writes.
///
/// The other half — Link, OSC and MIDI clock — is `output::Transports` in core, shared
/// with the application so there is one implementation of what a beat does to the
/// outputs rather than one per front end. Neither half knows about the other.
class BeatPrinter {
public:
    /// Writes every beat as "<seconds>\t<beat in bar>\t<BPM>". The first two columns are
    /// the format the beat-tracking datasets annotate in, so an estimate and a reference
    /// are the same kind of file and the same reader handles both; the third is what the
    /// tempo state machine was publishing at that beat, which is the only thing §5.5's
    /// octave fold can move. A fold cannot change a beat time, so it cannot show up in
    /// beat F-measure — the tempo column is where its effect is visible at all.
    void writeTo(const std::filesystem::path& path) {
        out_.open(path, std::ios::trunc);
        if (!out_) {
            throw std::runtime_error(path.string() + ": cannot create");
        }
        out_ << std::fixed << std::setprecision(6);
    }

    /// Always on the thread that owns stdout, never on the output thread: this console
    /// prints a status line on its own clock, and `operator<<` chains from two threads
    /// would interleave inside a line. Live, `runTrackLive` queues the beats and calls
    /// this from its loop — which is also what keeps file I/O off the output thread,
    /// where it would land as jitter on the MIDI clock.
    void print(const takt4::engine::EngineBeat& beat) {
        std::cout << beatLine(beat.event, beat.state) << '\n';
        if (out_.is_open()) {
            out_ << beat.event.time << '\t' << beat.event.beatInBar << '\t' << beat.event.bpm
                 << '\n';
        }
    }

private:
    std::ofstream out_;
};

/// What the console's arguments ask the transports to open.
takt4::output::Transports::Config transportConfig(const TrackArgs& args) {
    takt4::output::Transports::Config config;
    config.link = args.link;
    config.oscPrefix = args.oscPrefix;
    config.oscTargets = args.oscTargets;
    config.midiClockPort = args.midiClockPort;
    config.latencySeconds = args.tempo.latencyOffsetSeconds;
    return config;
}

/// The engine's options from the console's arguments.
takt4::engine::BeatEngine::Options engineOptions(const TrackArgs& args) {
    takt4::engine::BeatEngine::Options options;
    options.filter.seed = args.seed;
    options.tempo = args.tempo;
    return options;
}
/// Drains the frame ring and returns how many were on it.
///
/// The console has no use for the per-frame trace — that is §5.9's scrolling activation
/// display and the window's business — but a ring nobody drains fills up and the engine
/// would rightly start counting frames lost. The *beat* ring is not touched here:
/// `output::OutputRunner` is its single consumer.
std::size_t discardFrames(takt4::engine::BeatEngine& engine) {
    takt4::engine::EngineFrame frame;
    std::size_t frames = 0;
    while (engine.popFrame(frame)) {
        ++frames;
    }
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
    takt4::output::Transports transports(transportConfig(offline));
    BeatPrinter printer;
    if (args.beatsOut) {
        printer.writeTo(*args.beatsOut);
    }
    std::cout << in.string() << ": " << hops << " hops, weights "
              << weights.path().filename().string() << ", fold " << fixed1(args.tempo.minBpm) << "-"
              << fixed1(args.tempo.maxBpm) << " BPM, seed " << args.seed << '\n';
    if (args.anyOutput()) {
        std::cout << "note: Link, OSC and MIDI clock are driven from a live input only; "
                     "over a file this prints beats and nothing else.\n";
    }

    // Offline there is no output thread: a file is worked through as fast as it reads, so
    // there is nothing to be punctual for and everything happens on this thread in order.
    std::size_t frames = 0;
    for (std::size_t h = 0; h < hops; ++h) {
        engine->processHop(padded.data() + h * kHopSize, h);
        (void)engine->step();
        frames += discardFrames(*engine);
        // The "now" a transport would tick on is the audio's own time; nothing is enabled
        // here, but passing anything else would be a lie in the argument.
        const double now = static_cast<double>(engine->framesTracked()) * model.secondsPerFrame();
        takt4::engine::EngineBeat beat;
        while (engine->popBeat(beat)) {
            transports.publish(beat.event, beat.hostMicros, now);
            printer.print(beat);
        }
        transports.advance(now, engine->state());
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
    takt4::output::Transports transports(transportConfig(args));
    BeatPrinter printer;
    if (args.beatsOut) {
        printer.writeTo(*args.beatsOut);
    }
    // HANDOFF §4.3: the audio thread stamps each hop through Link's regression, so a
    // beat carries the host time of the audio it was found in rather than of the moment
    // this loop happened to notice it. Before the stream is started, because the stamp is
    // taken on the audio thread and there has to be a clock in place before there is one.
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

    // §5.5's manual controls. Settings are changed by reading what the engine has,
    // editing that, and posting it back — never by editing a copy kept since startup,
    // because a tap moves the octave-fold window and a stale copy would undo it on the
    // next key. A UI does the same thing on its redraw timer.
    takt4::tracking::TapTempo taps;
    KeyReader keys;
    printKeys(std::cout, keys.interactive());

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    engine->start();
    stream.start();

    const auto start = std::chrono::steady_clock::now();

    using takt4::engine::Command;
    const auto onKey = [&](int key, double elapsed) {
        switch (key) {
        case ' ': {
            const std::optional<double> tapped = taps.tap(elapsed);
            if (tapped) {
                (void)engine->post(Command::seedTempo(*tapped));
                std::cout << "  tap: " << fixed1(*tapped) << " BPM, fold window follows it\n";
            } else {
                std::cout << "  tap " << taps.taps() << " of " << taps.options().needTaps << "\n";
            }
            break;
        }
        case 'd':
            (void)engine->post(Command::snapDownbeat());
            std::cout << "  downbeat: the next beat starts the bar\n";
            break;
        case 'h':
            (void)engine->post(Command::halve());
            std::cout << "  /2\n";
            break;
        case 'x':
            (void)engine->post(Command::redouble());
            std::cout << "  *2\n";
            break;
        case '[':
        case ']': {
            // A relative nudge reads what is there and adds to it, so two presses landing
            // in the same 1 ms round would both read the same value and the queue would
            // keep only the second — one step of five milliseconds lost, and visible in
            // the line below, which prints the same number twice. Key repeat is 33 ms
            // apart, so it takes a deliberate double-tap. A UI with a real slider sends an
            // absolute value and does not have the question.
            takt4::tracking::TempoTracker::Options live = engine->tempoOptions();
            live.latencyOffsetSeconds += key == '[' ? -0.005 : 0.005;
            (void)engine->post(Command::setTempoOptions(live));
            transports.setLatencySeconds(live.latencyOffsetSeconds);
            std::cout << "  latency offset " << fixed1(live.latencyOffsetSeconds * 1000.0)
                      << " ms\n";
            break;
        }
        case 'f': {
            takt4::tracking::TempoTracker::Options live = engine->tempoOptions();
            live.octaveFold = !live.octaveFold;
            (void)engine->post(Command::setTempoOptions(live));
            std::cout << "  octave fold " << (live.octaveFold ? "on" : "off") << ", window "
                      << fixed1(live.minBpm) << "-" << fixed1(live.maxBpm) << " BPM\n";
            break;
        }
        case '?':
            printKeys(std::cout, true);
            break;
        case 'q':
        case 3: // Ctrl+C, which _getch() hands over as a character rather than a signal
            g_interrupted.store(true);
            break;
        default:
            break;
        }
    };

    // The output thread hands its beats over rather than printing them: stdout already has
    // a writer in this loop, and `operator<<` chains from two threads interleave inside a
    // line. It also keeps the beats file off the output thread, where writing it would
    // land as jitter on the clock it is trying to keep.
    //
    // Declared *before* the runner so they outlive it: the runner's observer holds
    // references to both, and a destructor joins its thread. Reversing these two would
    // leave the thread running against a destroyed queue if the scope unwound.
    std::mutex publishedMutex;
    std::vector<takt4::engine::EngineBeat> published;

    // §4.2's output thread. It is the single consumer of the engine's beat ring from here
    // on, so this loop must not drain it: it polls keys, prints, and drains the *frame*
    // ring, which nothing else wants. The tracking waits on neither — the inference thread
    // runs at the audio's pace — and now neither does a MIDI tick.
    takt4::output::OutputRunner runner(*engine, transports);
    runner.setBeatObserver([&](const takt4::engine::EngineBeat& beat) {
        const std::lock_guard<std::mutex> lock(publishedMutex);
        published.push_back(beat);
    });
    runner.start();

    auto lastStatus = start;
    std::vector<takt4::engine::EngineBeat> toPrint;
    // Nothing here is punctual any more, so this can be lazy: it decides how promptly a
    // beat is *printed*, not how promptly it is sent.
    constexpr auto period = 10ms;
    while (!shouldStop(start, args.beats.stream.seconds)) {
        std::this_thread::sleep_for(period);
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        for (int key = keys.poll(); key != 0; key = keys.poll()) {
            onKey(key, elapsed);
        }
        (void)discardFrames(*engine);
        {
            const std::lock_guard<std::mutex> lock(publishedMutex);
            toPrint.swap(published);
        }
        for (const takt4::engine::EngineBeat& beat : toPrint) {
            printer.print(beat);
        }
        toPrint.clear();
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
    // The runner before the host time source: it is still sending, and Link is what it
    // sends to. Its stop() drains the last beats and stops the transports.
    runner.stop();
    engine->setHostTimeSource(nullptr);
    (void)discardFrames(*engine);
    {
        const std::lock_guard<std::mutex> lock(publishedMutex);
        toPrint.swap(published);
    }
    for (const takt4::engine::EngineBeat& beat : toPrint) {
        printer.print(beat);
    }

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
            // The platform's timer resolution is `output::OutputRunner`'s to raise now,
            // because its thread is the only thing here that needs a millisecond to mean
            // one — and it holds it for exactly as long as the outputs are running.
            return runTrack({args.begin() + 1, args.end()});
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
