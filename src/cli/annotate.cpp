// takt4-cli annotate — annotate a track by tapping along to it.
//
// The project has no human ground truth for the music it is for: every score on the 23
// tracks is agreement with Beat This!, and on eight of them the reference systems
// disagree with each other. This is how the operator supplies it. The track plays, the
// operator taps space on the beats and d on the downbeats, and the taps become a `.beats`
// file beside the audio in the layout every other annotated set uses — so it feeds
// tools/train/layout.py as a set of its own, and tools/refeval/score.py as a reference.
//
// Two things make it worth more than a thumb. The network's activation is read over the
// whole file first, and every tap within 60 ms of one of its peaks is moved onto the peak,
// which is where every other set's labels sit and sharper than any tap; the taps that find
// no peak are moved by the median of how far the others moved, the operator's own lateness.
// And h / x set the octave the *file* is written at, so a half-time tap on drum and bass
// records the operator's octave as the label rather than the genre's.
//
// The taps are timed against the audio at the speakers, not against the moment a sample was
// handed to the driver: `PlaybackStream` stamps each buffer with its DAC time and the tap
// reads the same clock, so output latency cancels. A tap file is written beside the
// annotation, so the same taps can be redone with other settings (`--taps`).

#include "cli/annotate.hpp"

#include "cli/console.hpp"
#include "core/audio/playback_stream.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/activation_engine.hpp"
#include "core/model/weights.hpp"
#include "core/tracking/annotation.hpp"
#include "core/tracking/tap_tempo.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace takt4::cli {

namespace {

using namespace std::chrono_literals;

struct AnnotateArgs {
    std::filesystem::path in;
    std::optional<std::filesystem::path> out;
    std::optional<std::filesystem::path> tapsIn;
    std::optional<std::filesystem::path> tapsOut;
    std::string weights = "electronic";
    tracking::AnnotationOptions options;
    std::optional<std::string> output;
    double startSeconds = 0.0;
    bool listOutputs = false;
};

std::string fixed(double value, int places) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(places) << value;
    return out.str();
}

double parseDouble(std::string_view text, std::string_view what) {
    std::size_t consumed = 0;
    const std::string s(text);
    double value = 0.0;
    try {
        value = std::stod(s, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(what) + ": expected a number, got '" + s + "'");
    }
    // `stod` reads "nan" and "inf" as numbers, which no option here can mean: `--snap nan`
    // turned snapping off. The console's own parser refused them and annotate's did not (the
    // audit of 2026-09-25, L46).
    if (consumed != s.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string(what) + ": expected a number, got '" + s + "'");
    }
    return value;
}

AnnotateArgs parseArgs(const std::vector<std::string_view>& args) {
    AnnotateArgs out;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--out") {
            out.out = std::filesystem::path(value());
        } else if (arg == "--taps") {
            out.tapsIn = std::filesystem::path(value());
        } else if (arg == "--save-taps") {
            out.tapsOut = std::filesystem::path(value());
        } else if (arg == "--weights") {
            out.weights = std::string(value());
        } else if (arg == "--snap") {
            const std::string_view how = value();
            out.options.snapWindowSeconds = how == "off" ? 0.0 : parseDouble(how, arg) / 1000.0;
        } else if (arg == "--octave") {
            const std::string_view which = value();
            if (which == "half") {
                out.options.octave = -1;
            } else if (which == "double") {
                out.options.octave = 1;
            } else if (which == "same") {
                out.options.octave = 0;
            } else {
                throw std::invalid_argument("--octave expects half, double or same");
            }
        } else if (arg == "--meter") {
            // Checked as a number before it is made an int: 1e10 cast to one is undefined, and
            // 4.5 was quietly a bar of four (L46).
            const double meter = parseDouble(value(), arg);
            if (!(meter >= 1.0 && meter <= 16.0) || meter != std::floor(meter)) {
                throw std::invalid_argument("--meter expects a whole number from 1 to 16");
            }
            out.options.defaultMeter = static_cast<int>(meter);
        } else if (arg == "--output") {
            out.output = std::string(value());
        } else if (arg == "--start") {
            out.startSeconds = parseDouble(value(), arg);
        } else if (arg == "--outputs") {
            out.listOutputs = true;
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("annotate: unknown option " + std::string(arg));
        } else if (out.in.empty()) {
            out.in = std::filesystem::path(arg);
        } else {
            throw std::invalid_argument("annotate takes one file");
        }
    }
    if (out.in.empty() && !out.listOutputs) {
        throw std::invalid_argument("annotate needs IN.wav");
    }
    return out;
}

/// P(beat) + P(downbeat) for every network frame of the file, by frame index: the
/// engine's own path, stepped on this thread, as `beats` over a file does it.
std::vector<float> listenAhead(const io::WavData& audio, const model::ModelWeights& weights) {
    using audio::kHopSize;
    const std::size_t hops = (audio.samples.size() + kHopSize - 1) / kHopSize;
    std::vector<float> padded(hops * kHopSize, 0.0f);
    std::copy(audio.samples.begin(), audio.samples.end(), padded.begin());
    model::ActivationEngine engine(weights);
    std::vector<float> activation;
    activation.reserve(hops);
    model::FrameActivation frame;
    for (std::size_t h = 0; h < hops; ++h) {
        engine.processHop(padded.data() + h * kHopSize, h);
        (void)engine.step();
        while (engine.pop(frame)) {
            const std::size_t index = static_cast<std::size_t>(frame.frameIndex);
            if (activation.size() <= index) {
                activation.resize(index + 1, 0.0f);
            }
            activation[index] = frame.beat + frame.downbeat;
        }
    }
    return activation;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// `--output`'s device: an index from `--outputs`, or a substring of a name.
audio::OutputDevice pickOutput(const audio::PortAudioSession& session,
                               const std::optional<std::string>& spec) {
    if (!spec) {
        const std::optional<audio::OutputDevice> fallback = audio::defaultOutputDevice(session);
        if (!fallback) {
            throw std::runtime_error("no output device on this machine; annotate from a tap file with --taps");
        }
        return *fallback;
    }
    const std::vector<audio::OutputDevice> devices = audio::listOutputDevices(session);
    const bool numeric = !spec->empty() && std::all_of(spec->begin(), spec->end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    if (numeric) {
        const int index = std::stoi(*spec);
        for (const audio::OutputDevice& device : devices) {
            if (device.index == index) {
                return device;
            }
        }
        throw std::invalid_argument("--output: no output device with index " + *spec);
    }
    const std::string needle = lower(*spec);
    std::vector<audio::OutputDevice> matches;
    for (const audio::OutputDevice& device : devices) {
        if (lower(device.name).find(needle) != std::string::npos) {
            matches.push_back(device);
        }
    }
    if (matches.empty()) {
        throw std::invalid_argument("--output: no output device named like '" + *spec + "'");
    }
    // Several host APIs list the same endpoint; WASAPI's is the one that stamps DAC times.
    const std::optional<audio::OutputDevice> preferWasapi = [&]() -> std::optional<audio::OutputDevice> {
        for (const audio::OutputDevice& device : matches) {
            if (device.hostApiName.find("WASAPI") != std::string::npos) {
                return device;
            }
        }
        return std::nullopt;
    }();
    return preferWasapi ? *preferWasapi : matches.front();
}

int listOutputs() {
    const audio::PortAudioSession session;
    const std::optional<audio::OutputDevice> chosen = audio::defaultOutputDevice(session);
    std::cout << "output devices:\n";
    for (const audio::OutputDevice& device : audio::listOutputDevices(session)) {
        std::cout << "  " << std::setw(3) << device.index << "  " << device.hostApiName << " / "
                  << device.name << "  (" << device.maxOutputChannels << " out @ "
                  << fixed(device.defaultSampleRate, 0) << " Hz)"
                  << (chosen && chosen->index == device.index ? "  <- default" : "") << '\n';
    }
    return 0;
}

#if defined(_WIN32)
/// A millisecond that means one: without this the loop's sleep resolves to 15.6 ms on
/// Windows, and that would be the tap's precision. The output thread raises it the same
/// way while the transports run.
struct TimerResolution {
    TimerResolution() { timeBeginPeriod(1); }
    ~TimerResolution() { timeEndPeriod(1); }
    TimerResolution(const TimerResolution&) = delete;
    TimerResolution& operator=(const TimerResolution&) = delete;
};
#else
struct TimerResolution {};
#endif

void printKeys() {
    std::cout << "keys:      space beat · d downbeat · u undo the last tap · s snap to the network's\n"
                 "           peaks on/off · h write the file at half time · x at double time ·\n"
                 "           q finish and write\n";
}

/// The session at the console: the track plays, the keys become taps.
std::vector<tracking::Tap> tapAlong(const io::WavData& audio, AnnotateArgs& a) {
    KeyReader keys;
    if (!keys.interactive()) {
        throw std::runtime_error("annotate needs a console to tap in — stdin is not one (MinTTY, "
                                 "which Git Bash uses, is not a console; cmd, PowerShell and Windows "
                                 "Terminal are). Or give the taps as a file: --taps FILE");
    }
    const audio::PortAudioSession session;
    const audio::OutputDevice device = pickOutput(session, a.output);
    audio::PlaybackStream player(session, device, audio.samples, static_cast<double>(audio.sampleRate),
                                 a.startSeconds);
    std::cout << "playing:   " << device.hostApiName << " / " << device.name << " at "
              << fixed(player.deviceSampleRate(), 0) << " Hz, " << player.channels()
              << (player.channels() == 1 ? " channel" : " channels") << ", output latency "
              << fixed(player.outputLatencySeconds() * 1000.0, 1) << " ms\n"
              << "track:     " << fixed(player.durationSeconds(), 1) << " s"
              << (a.startSeconds > 0.0 ? ", from " + fixed(a.startSeconds, 1) + " s" : "") << '\n';
    printKeys();
    std::cout << std::flush;

    [[maybe_unused]] const TimerResolution resolution;
    // A Ctrl+C from here finishes the session as q does — the taps written — rather than
    // ending the process with them: the caller's `Interrupts` is catching it.
    std::vector<tracking::Tap> taps;
    tracking::TapTempo counting;
    player.start();
    auto lastStatus = std::chrono::steady_clock::now();
    bool done = false;
    while (!done) {
        std::this_thread::sleep_for(1ms);
        for (int key = keys.poll(); key != 0; key = keys.poll()) {
            switch (key) {
            case ' ':
            case 'd':
            case 'D': {
                const double at = player.positionSeconds();
                taps.push_back(tracking::Tap{at, key != ' '});
                const std::optional<double> tempo = counting.tap(at);
                std::cout << "  " << (key == ' ' ? "beat    " : "DOWNBEAT") << "  " << fixed(at, 3) << " s"
                          << (tempo ? "  ~" + fixed(*tempo, 1) + " BPM" : "") << "   (" << taps.size()
                          << (taps.size() == 1 ? " tap)" : " taps)") << '\n';
                break;
            }
            case 'u':
            case 'U':
                if (!taps.empty()) {
                    std::cout << "  undone: the tap at " << fixed(taps.back().seconds, 3) << " s\n";
                    taps.pop_back();
                    counting.reset();
                }
                break;
            case 's':
            case 'S':
                a.options.snapWindowSeconds = a.options.snapWindowSeconds > 0.0 ? 0.0 : 0.06;
                std::cout << "  snap " << (a.options.snapWindowSeconds > 0.0 ? "on: taps within 60 ms of a peak go to it"
                                                                             : "off: taps stay where they landed")
                          << '\n';
                break;
            case 'h':
            case 'H':
                a.options.octave = a.options.octave == -1 ? 0 : -1;
                std::cout << "  file at " << (a.options.octave == -1 ? "half time: every other tap kept"
                                                                     : "the tapped tempo")
                          << '\n';
                break;
            case 'x':
            case 'X':
                a.options.octave = a.options.octave == 1 ? 0 : 1;
                std::cout << "  file at " << (a.options.octave == 1 ? "double time: a beat between every two taps"
                                                                    : "the tapped tempo")
                          << '\n';
                break;
            case 'q':
            case 'Q':
                done = true;
                break;
            default:
                break;
            }
        }
        if (player.finished()) {
            std::cout << "  the track has ended\n";
            done = true;
        }
        if (Interrupts::requested() && !done) {
            std::cout << "  interrupted: writing what was tapped\n";
            done = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatus >= 2s) {
            lastStatus = now;
            std::size_t downbeats = 0;
            for (const tracking::Tap& tap : taps) {
                downbeats += tap.downbeat ? 1 : 0;
            }
            std::cout << "         " << fixed(player.positionSeconds(), 1) << " / "
                      << fixed(player.durationSeconds(), 1) << " s   " << taps.size() << " taps, "
                      << downbeats << " downbeats"
                      << (counting.bpm() > 0.0 ? "   ~" + fixed(counting.bpm(), 1) + " BPM" : "") << '\n';
        }
        std::cout << std::flush;
    }
    player.stop();
    return taps;
}

} // namespace

void printAnnotateUsage(std::ostream& out) {
    out << "  takt4-cli annotate IN.wav [--out FILE] [--taps FILE] [--save-taps FILE]\n"
        << "                  [--weights SET|PATH] [--snap MS|off] [--octave half|double]\n"
        << "                  [--meter N] [--output DEVICE] [--start S] [--outputs]\n"
        << "      Play a mono " << audio::kInternalSampleRate
        << " Hz WAV and tap along: space on the beats, d on the\n"
        << "      downbeats, q to finish. Writes the taps as a beat annotation — <seconds>\n"
        << "      TAB <beat in bar>, downbeat 1, the layout the datasets and the harness\n"
        << "      use — to --out, or <IN>.beats beside the file; and the raw taps to\n"
        << "      --save-taps, or <out>.taps beside it, so they can be redone.\n"
        << "      --taps FILE     no playback: the taps from a file (\"<seconds>[ TAB d]\",\n"
        << "                      or any .beats file), snapped and written as above\n"
        << "      --snap MS       move a tap within MS of a peak of the network's activation\n"
        << "                      onto the peak (default 60; off leaves the taps alone);\n"
        << "                      taps that find no peak move by the others' median lateness\n"
        << "      --octave O      half writes every other tap, the operator's half-time grid\n"
        << "                      as the label; double puts a beat between every two\n"
        << "      --meter N       the bar when fewer than two downbeats were tapped (4)\n"
        << "      --output D      play through this device: an index from --outputs, or a\n"
        << "                      substring of its name; default the system's output\n"
        << "      --start S       begin playback at S seconds; times stay absolute\n"
        << "      --outputs       list the output devices and exit\n"
        << "\n";
}

int runAnnotate(const std::vector<std::string_view>& args) {
    AnnotateArgs a = parseArgs(args);
    if (a.listOutputs) {
        return listOutputs();
    }
    const io::WavData audio = io::readWavFile(a.in);
    if (audio.channels != 1 || static_cast<double>(audio.sampleRate) != audio::kInternalSampleRate) {
        throw std::invalid_argument(a.in.string() + ": expected mono at " +
                                    std::to_string(static_cast<int>(audio::kInternalSampleRate)) +
                                    " Hz (tools/annotate.py decodes anything else)");
    }
    const std::filesystem::path outPath =
        a.out.value_or(a.in.parent_path() / (a.in.stem().string() + ".beats"));
    const std::filesystem::path tapsPath =
        a.tapsOut.value_or(outPath.parent_path() / (outPath.stem().string() + ".taps"));

    const model::ModelWeights weights = loadWeights(a.weights);
    std::cout << a.in.string() << ": " << fixed(static_cast<double>(audio.samples.size()) / audio::kInternalSampleRate, 1)
              << " s, weights " << weights.path().filename().string() << "\nlistening ahead for the peaks... "
              << std::flush;
    const std::vector<float> activation = listenAhead(audio, weights);
    std::cout << activation.size() << " frames\n";

    std::vector<tracking::Tap> taps;
    // Made here rather than in `tapAlong`, so Ctrl+C is still caught while the taps are written
    // below: made there, it was gone by then, and a second press lost them (the audit of
    // 2026-09-25, L47).
    std::optional<Interrupts> interrupts;
    if (a.tapsIn) {
        taps = tracking::readTaps(*a.tapsIn);
        std::cout << taps.size() << " taps from " << a.tapsIn->string() << '\n';
    } else {
        interrupts.emplace();
        taps = tapAlong(audio, a);
    }
    if (taps.empty()) {
        std::cout << "no taps; nothing written\n";
        return 1;
    }
    if (!a.tapsIn || a.tapsOut) {
        tracking::writeTaps(tapsPath, taps);
        std::cout << "taps:      " << tapsPath.string() << '\n';
    }

    const tracking::Annotation annotation = tracking::annotate(taps, activation, a.options);
    tracking::writeBeats(outPath, annotation);
    const auto& s = annotation.stats;
    std::cout << "written:   " << outPath.string() << '\n'
              << "beats:     " << annotation.times.size() << " (" << s.taps << " taps"
              << (a.options.octave < 0 ? ", every other one kept" : a.options.octave > 0 ? ", midpoints added" : "")
              << "), " << s.downbeats << " downbeats, "
              << (s.meter > 0 ? std::to_string(s.meter) + " to the bar" : std::string("bar unknown: no downbeat tapped"))
              << ", ~" << fixed(s.bpm, 1) << " BPM\n";
    if (a.options.snapWindowSeconds > 0.0) {
        std::cout << "snap:      " << s.snapped << " of " << s.taps << " taps found a peak within "
                  << fixed(a.options.snapWindowSeconds * 1000.0, 0) << " ms; the thumb ran "
                  << fixed(s.medianOffsetSeconds * 1000.0, 0) << " ms " << (s.medianOffsetSeconds >= 0.0 ? "late" : "early")
                  << " (10-90 % spread " << fixed(s.spreadSeconds * 1000.0, 0) << " ms)"
                  << (s.snapped == 0 ? "; nothing was moved" : s.snapped < s.taps ? "; the rest moved by the median" : "")
                  << '\n';
    } else {
        std::cout << "snap:      off; the taps stand where they landed\n";
    }
    if (s.downbeats == 0) {
        std::cout << "note:      no downbeat was tapped (d), so every beat is written with bar position 0;\n"
                     "           a training set wants the downbeats, the beat harness does not\n";
    }
    return 0;
}

} // namespace takt4::cli
