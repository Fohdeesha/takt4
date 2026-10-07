#include "ui/window_state.hpp"

#include "core/audio/hop_meter.hpp"
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
    const std::from_chars_result result = std::from_chars(begin, end, value);
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

void publishTempoOptions(MainWindow& window, const tracking::TempoTracker::Options& options) {
    window.set_fold_on(options.octaveFold);
    window.set_fold_min(static_cast<float>(options.minBpm));
    window.set_fold_max(static_cast<float>(options.maxBpm));
    window.set_latency_ms(static_cast<float>(options.latencyOffsetSeconds * 1000.0));
    window.set_keep_shift(options.keepOctaveShift);
}

void publishControlLimits(MainWindow& window) {
    window.set_fold_limit_min(static_cast<float>(kFoldFloorBpm));
    window.set_fold_limit_max(static_cast<float>(kFoldCeilingBpm));
    window.set_fold_least_span(static_cast<float>(kFoldLeastSpanBpm));
    window.set_latency_limit_ms(static_cast<float>(kLatencyLimitMs));
    // Per-output delay, which is a different control from §5.5's latency above: that one
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
