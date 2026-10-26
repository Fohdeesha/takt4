// Hours of a show through the engine and the output thread, as fast as the machine runs them:
// music, a stop long enough to be no signal, music again, beatless noise, over and over — every
// way into and out of a lock, many hundreds of times. What a gig of several hours does to memory,
// handles and beats, found in minutes rather than at the end of a long night.
//
// Hidden: `takt4_tests "[soak]"`. TAKT4_SOAK_SECONDS sets the hours (seconds of audio; default an
// hour). Not real time — the audio goes in as fast as it is tracked and the beats fire as they
// are heard (no clock to schedule them by), so this finds what grows, what is dropped and what
// the tracker carries from one lap into the next — the last lap's beats are held against the
// second's, frame for frame — but not what drifts against a wall clock.

#include "core/audio/rates.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/output_runner.hpp"
#include "core/output/transports.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/state_space.hpp"
#include "core/trigger/rule.hpp"

#include "support/loopback_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
// After windows.h.
#include <psapi.h>
#else
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#endif

using takt4::audio::kHopSize;
using takt4::engine::BeatEngine;
using takt4::output::OutputCommand;
using takt4::output::OutputRunner;
using takt4::output::Transports;

namespace {

const std::filesystem::path kTestData{TAKT4_TEST_DATA_DIR};
const std::filesystem::path kWeightsDir{TAKT4_WEIGHTS_DIR};

double soakSeconds() {
    std::string text;
#if defined(_MSC_VER)
    char buffer[32] = {};
    std::size_t length = 0;
    if (getenv_s(&length, buffer, sizeof buffer, "TAKT4_SOAK_SECONDS") == 0 && length > 1) {
        text = buffer;
    }
#else
    if (const char* value = std::getenv("TAKT4_SOAK_SECONDS")) {
        text = value;
    }
#endif
    return text.empty() ? 3600.0 : std::stod(text);
}

/// The process's own memory, private bytes, and its open handles (or file descriptors).
struct Use {
    double megabytes = 0.0;
    std::size_t handles = 0;
};

Use useNow() {
    Use use;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                               sizeof counters) != 0) {
        use.megabytes = static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
    }
    DWORD handles = 0;
    if (::GetProcessHandleCount(::GetCurrentProcess(), &handles) != 0) {
        use.handles = handles;
    }
#else
    std::ifstream statm("/proc/self/statm");
    long pages = 0;
    long resident = 0;
    if (statm >> pages >> resident) {
        use.megabytes = static_cast<double>(resident) * static_cast<double>(::sysconf(_SC_PAGESIZE)) /
                        (1024.0 * 1024.0);
    }
    if (DIR* fds = ::opendir("/dev/fd")) {
        while (::readdir(fds) != nullptr) {
            ++use.handles;
        }
        ::closedir(fds);
    }
#endif
    return use;
}

std::vector<float> excerpt(const char* name) {
    return takt4::io::readWavFile(kTestData / "features" / name).samples;
}

/// Loud pink noise, `seconds` of it, from a fixed seed: a crowd or a pad, no beat in it.
std::vector<float> noise(double seconds) {
    std::vector<float> out(static_cast<std::size_t>(seconds * takt4::audio::kInternalSampleRate));
    std::uint32_t state = 2463534242U;
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    for (float& sample : out) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        const double white = static_cast<double>(state) / 4294967295.0 * 2.0 - 1.0;
        b0 = 0.99765 * b0 + white * 0.0990460;
        b1 = 0.96300 * b1 + white * 0.2965164;
        b2 = 0.57000 * b2 + white * 1.0526913;
        sample = static_cast<float>(0.1 * (b0 + b1 + b2 + white * 0.1848));
    }
    return out;
}

} // namespace

TEST_CASE("hours of a show leave memory, handles and the beats where an hour of it left them",
          "[.][soak]") {
    // One lap: a record, the next, the deck stopped long enough for no signal, another record,
    // a stretch of loud noise, and on.
    std::vector<float> lap;
    const auto add = [&lap](const std::vector<float>& part) { lap.insert(lap.end(), part.begin(), part.end()); };
    const std::vector<float> pirates = excerpt("pirates.wav");
    add(pirates);
    add(excerpt("in-yer-face.wav"));
    add(std::vector<float>(static_cast<std::size_t>(6.0 * takt4::audio::kInternalSampleRate), 0.0f));
    add(excerpt("good-times.wav"));
    add(noise(8.0));
    add(excerpt("your-sweet-boom.wav"));
    lap.resize(lap.size() / kHopSize * kHopSize);
    const double lapSeconds = static_cast<double>(lap.size()) / takt4::audio::kInternalSampleRate;

    // The configuration that ships, the outputs a rig has: the namespace to a monitor, a rule on
    // every beat with a release a beat later, one on every bar.
    static const takt4::model::ModelWeights electronic =
        takt4::model::ModelWeights::fromFile(kWeightsDir / "electronic.bin");
    static const takt4::tracking::StateSpaceModel stateSpace = takt4::tracking::StateSpaceModel::fromFile(
        std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin");
    const takt4::settings::Settings fresh;
    BeatEngine::Options options;
    options.tempo = fresh.preset.tempo;
    options.decoder = fresh.preset.decoder;
    options.forward.meters = fresh.preset.meters;
    BeatEngine engine(electronic, stateSpace, options);

    takt4::testing::LoopbackReceiver monitor;
    Transports::Config config;
    takt4::output::OutputTarget target;
    target.name = "monitor";
    target.host = "127.0.0.1";
    target.port = monitor.port();
    target.sendsNamespace = true;
    config.outputs = {target};
    OutputRunner runner(engine, config);
    takt4::trigger::Rule::Config beat;
    beat.id = "beat";
    beat.trigger = takt4::trigger::Trigger::Beat;
    beat.address = "/beat";
    takt4::trigger::FollowUp release;
    release.unit = takt4::trigger::DelayUnit::Beats;
    release.delayBeats = 1.0;
    beat.followUps.push_back(release);
    takt4::trigger::Rule::Config bar;
    bar.id = "bar";
    bar.trigger = takt4::trigger::Trigger::Bar;
    bar.address = "/bar";
    runner.post(OutputCommand::rules({beat, bar}));

    // Whatever reaches the monitor, read and thrown away as it comes, as a receiver would.
    std::atomic<bool> listening{true};
    std::atomic<std::uint64_t> received{0};
    std::thread reader([&] {
        while (listening.load(std::memory_order_acquire)) {
            if (monitor.ready(20)) {
                (void)monitor.receive();
                received.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    // Stopped and joined however this test ends: a REQUIRE that fails part-way through would
    // otherwise destroy a thread still running, which ends the process.
    struct StopsReading {
        std::atomic<bool>& listening;
        std::thread& reader;
        ~StopsReading() {
            listening.store(false, std::memory_order_release);
            if (reader.joinable()) {
                reader.join();
            }
        }
    } stopsReading{listening, reader};

    runner.start();
    const double total = soakSeconds();
    const std::size_t laps = std::max<std::size_t>(4, static_cast<std::size_t>(std::ceil(total / lapSeconds)));
    std::vector<Use> uses;
    uses.reserve(laps);
    std::vector<std::uint64_t> beatsPerLap;
    beatsPerLap.reserve(laps);
    // Where the second lap's beats fell, and the last whole lap's: the frame inside the lap,
    // counted from the first frame the tracker gave, and where inside that frame. The same audio
    // every lap, so the same places, unless something the tracker keeps is not put back where it
    // was when the signal goes. Two laps' worth and no more: every lap's would be memory this test
    // grew by itself, which the check on memory below would read as a leak.
    const std::uint64_t framesPerLap = lap.size() / kHopSize * engine.stepsPerActivation();
    std::vector<std::pair<std::uint64_t, double>> second;
    std::vector<std::pair<std::uint64_t, double>> later;
    second.reserve(1024);
    later.reserve(1024);
    std::uint64_t framesSeen = 0;
    takt4::engine::EngineFrame frame;
    std::uint64_t hop = 0;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t l = 0; l < laps; ++l) {
        const std::uint64_t beatsBefore = engine.beatsCalled();
        for (std::size_t h = 0; h < lap.size() / kHopSize; ++h) {
            engine.processHop(lap.data() + h * kHopSize, hop++);
            (void)engine.step();
            while (engine.popFrame(frame)) {
                const std::uint64_t inLap = framesSeen / framesPerLap;
                if (frame.beat && (inLap == 1 || inLap == laps - 2)) {
                    (inLap == 1 ? second : later)
                        .emplace_back(framesSeen % framesPerLap, frame.tracked.beatOffsetFrames);
                }
                ++framesSeen;
            }
        }
        beatsPerLap.push_back(engine.beatsCalled() - beatsBefore);
        uses.push_back(useNow());
    }
    REQUIRE(runner.sync());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    runner.stop();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // From the second lap on: the first fills every pool and cache there is to fill. Measured over
    // six hours of audio, twice, private bytes moved by 0.05 MB and by 0.01; a leak of a hundred
    // bytes a beat would be 4.5 MB in those six hours.
    const Use& settled = uses[1];
    const Use& last = uses.back();
    INFO(laps << " laps of " << lapSeconds << " s, " << static_cast<double>(laps) * lapSeconds / 3600.0
              << " h of audio in "
              << took << " s; memory " << settled.megabytes << " MB after the first lap, "
              << last.megabytes << " MB at the end; handles " << settled.handles << " then "
              << last.handles << "; " << received.load() << " datagrams to the monitor");
    CHECK(last.megabytes - settled.megabytes < 4.0);
    CHECK(last.handles <= settled.handles + 8);
    CHECK(engine.beatsDropped() == 0);
    CHECK(engine.framesDropped() == 0);
    CHECK(runner.errors() == 0);
    // Every lap the same audio, so the same beats, to within a beat or two where a lock came a
    // frame either way.
    const auto [fewest, most] = std::minmax_element(beatsPerLap.begin() + 1, beatsPerLap.end());
    INFO("beats a lap: " << *fewest << " to " << *most);
    CHECK(*most - *fewest <= 4);
    CHECK(received.load() > 0);
    // And in the same places: the second lap's beats against the last whole one's (the last lap's
    // final frames may still have been on their way). Hours later, not a frame or a hair of one
    // apart.
    double widest = 0.0;
    std::size_t moved = 0;
    for (std::size_t i = 0; i < std::min(second.size(), later.size()); ++i) {
        if (second[i].first != later[i].first) {
            ++moved;
        }
        widest = std::max(widest, std::abs(second[i].second - later[i].second));
    }
    INFO("beats in the second lap " << second.size() << ", in lap " << laps - 1 << " " << later.size()
                                    << "; " << moved << " on another frame, offsets apart by up to "
                                    << widest << " of a frame");
    CHECK(second.size() == later.size());
    CHECK(moved == 0);
    CHECK(widest < 1e-6);
}
