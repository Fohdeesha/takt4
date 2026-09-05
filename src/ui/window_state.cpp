#include "ui/window_state.hpp"

#include "core/audio/hop_meter.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace takt4::ui {

std::string fixed(double value, int places) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(places) << value;
    return out.str();
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
    window.set_locked(state.locked);
    window.set_holding(state.holding);
    window.set_refined(state.refined);
    window.set_confidence(static_cast<float>(state.confidence));
    window.set_beats_per_bar(static_cast<int>(state.beatsPerBar));
    window.set_beat_in_bar(static_cast<int>(state.beatInBar));
    window.set_bars(static_cast<int>(state.bars));
}

void publishTempoOptions(MainWindow& window, const tracking::TempoTracker::Options& options) {
    window.set_fold_on(options.octaveFold);
    window.set_fold_min(static_cast<float>(options.minBpm));
    window.set_fold_max(static_cast<float>(options.maxBpm));
    window.set_latency_ms(static_cast<float>(options.latencyOffsetSeconds * 1000.0));
}

void publishControlLimits(MainWindow& window) {
    window.set_fold_limit_min(static_cast<float>(kFoldFloorBpm));
    window.set_fold_limit_max(static_cast<float>(kFoldCeilingBpm));
    window.set_fold_least_span(static_cast<float>(kFoldLeastSpanBpm));
    window.set_latency_limit_ms(static_cast<float>(kLatencyLimitMs));
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

} // namespace takt4::ui
