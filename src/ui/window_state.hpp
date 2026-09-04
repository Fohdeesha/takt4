#pragma once

#include "core/audio/devices.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "main_window.h" // generated from main_window.slint

#include <cstddef>
#include <string>

namespace takt4::ui {

/// Frames of activation the trace holds: four seconds at the 50 Hz frame rate, which is
/// two bars at 120 BPM — long enough to see the pattern the network is responding to.
inline constexpr std::size_t kTraceLength = 200;

/// The bottom of the input meter. Quieter than this is not a level anyone is setting.
inline constexpr float kMeterFloorDb = -60.0f;

/// Turning engine values into window properties, in one place.
///
/// The live window and `takt4-shot` both fill the same markup, and a property added to
/// `main_window.slint` should have exactly one place that learns to set it. Everything
/// here is pure: no tracker, no timer, no audio device — which is what lets the shot tool
/// render the window with no window system underneath it.

/// One frame of the activation trace.
TracePoint tracePoint(const engine::EngineFrame& frame);

/// The BPM readout, lock and confidence, and the bar.
void publishTempoState(MainWindow& window, const tracking::TempoState& state);

/// §5.5's settings. Read these from `BeatEngine::tempoOptions()` every time and never
/// from a copy: a tap moves the octave-fold window (§7 deviation 8).
void publishTempoOptions(MainWindow& window, const tracking::TempoTracker::Options& options);

/// Everything the readouts say while nothing is running, so a stopped window does not
/// leave the last set's tempo sitting there looking live.
void publishIdleReadouts(MainWindow& window);

/// The input meter, from one hop's linear RMS and a peak that the caller decays.
void publishInput(MainWindow& window, float rms, float peak);

/// What the device picker shows. The host API leads, because on Windows one interface
/// appears under both ASIO and WASAPI and the two are not interchangeable (§5.1): only
/// the ASIO entry picks channels natively, and only the WASAPI one offers loopback.
std::string describeDevice(const audio::InputDevice& device);

/// What the channel picker shows, numbered from 1 as it is printed on the interface.
/// ASIO and CoreAudio hand over the driver's own names for them; nothing else does.
std::string describeChannel(const audio::InputDevice& device, int channel);

/// One decimal place or two, without disturbing any stream's flags.
std::string fixed(double value, int places);

} // namespace takt4::ui
