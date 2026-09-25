#pragma once

#include "core/audio/devices.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "main_window.h" // generated from main_window.slint

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace takt4::ui {

/// What each window opens at, in logical pixels.
///
/// **Set from C++, not from the markup's `preferred-width`/`preferred-height`.** Slint sizes a
/// window from what its *content* asks for — `layout_info(...).preferred_bounded()` in the
/// winit backend — so the Window element's own preferred size does not decide anything, and
/// the rule editor opened at its `min-width` x `min-height` however large a preference the
/// markup declared. `Window::set_size` before the first `show()` is what settles it: it marks
/// the size explicit, and the backend then leaves it alone.
///
/// Only the first show. Whatever the operator drags a window to afterwards is theirs.
inline constexpr float kMainWindowWidth = 1000.0f;
/// Tall enough for the status bar — which holds the version — to be on screen with a couple
/// of output rows above it. Short of this the bottom row is simply cut off. 900 until the
/// "keep for the next track" row went in under the latency slider (the audit's H2); the 34 px
/// it takes are given back here, so five outputs and their ADD OUTPUT row still fit, and it
/// still opens whole on a 1080-line screen under a title bar and a taskbar.
inline constexpr float kMainWindowHeight = 934.0f;
/// Measured on a rig: what the editor was dragged to and asked to open at.
inline constexpr float kRulesWindowWidth = 1164.0f;
inline constexpr float kRulesWindowHeight = 872.0f;
/// And the patch editor, which had no size of its own at all and so opened at its markup
/// minimum — 820x520, which is too short to see a twelve-channel head's map and the movement
/// limits under it without scrolling. Measured from the shot a rig sent on 2026-09-16 asking
/// for "at least as big as the attached screenshot".
inline constexpr float kFixturesWindowWidth = 1080.0f;
inline constexpr float kFixturesWindowHeight = 800.0f;

/// Frames of activation the trace holds: four seconds at the 50 Hz frame rate, which is
/// two bars at 120 BPM — long enough to see the pattern the network is responding to.
inline constexpr std::size_t kTraceLength = 200;

/// The bottom of the input meter. Quieter than this is not a level anyone is setting.
inline constexpr float kMeterFloorDb = -60.0f;

/// How far the octave-fold sliders reach, and the least the two of them may be apart.
///
/// Wider than the state space's own 54.5-214.3 BPM on purpose: the window is what a
/// tempo is folded *into*, not what the filter can track, so a house set watched at
/// half time is a window of 60-90 over material the filter is following at 140.
/// `foldInto` returns the tempo unfolded when the window is inverted — a fold that
/// silently stops working — so the two never cross, here or in the markup.
inline constexpr double kFoldFloorBpm = 40.0;
inline constexpr double kFoldCeilingBpm = 220.0;
inline constexpr double kFoldLeastSpanBpm = 5.0;

/// §5.5's latency offset, in milliseconds either side of zero. Beyond half a beat the
/// control stops meaning anything — it is the next beat — and half a beat at 120 BPM is
/// 250 ms, so that is the end of the slider.
inline constexpr double kLatencyLimitMs = 250.0;

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

/// How far the settings above can be dragged, from the constants the caller also clamps
/// against — so a slider cannot offer a value the controller would refuse. Once, at
/// startup; nothing moves them afterwards.
void publishControlLimits(MainWindow& window);

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

/// A number of milliseconds as somebody typed it into a reading (`NumberEntry`): "12", "+12",
/// "-30.5", "12 ms", "12ms", with spaces anywhere around it. Nothing for anything else — the
/// caller says so rather than guessing. Not clamped: that is the caller's, against its own limit.
std::optional<double> readMilliseconds(std::string_view text);

} // namespace takt4::ui
