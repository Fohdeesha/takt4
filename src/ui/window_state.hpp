#pragma once

#include "core/audio/devices.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "main_window.h" // generated from main_window.slint

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
///
/// 800 wide since the Weltformat-dark redesign (HANDOFF §0.5, 2026-09-29): the operator cut the
/// width by a fifth, and every row of the outputs table is laid out to read whole at it.
inline constexpr float kMainWindowWidth = 800.0f;
/// It opens at this with a scroll bar rather than sized to its content (1146 px with five
/// outputs and their peers): §0.5 kept today's height, which opens whole on a 1080-line screen
/// under a title bar and a taskbar.
inline constexpr float kMainWindowHeight = 934.0f;
/// The markup's `min-height` — what folding a section never shrinks the window below.
inline constexpr float kMainWindowMinHeight = 420.0f;
/// The rule editor as the operator locked it on 2026-09-30 (HANDOFF §0.5): 1000 wide — the log
/// left its column for a sheet along the bottom, and the list took 46 px of that room — and the
/// 872 a rig measured and asked to open at.
inline constexpr float kRulesWindowWidth = 1000.0f;
inline constexpr float kRulesWindowHeight = 872.0f;
/// And the patch editor, which had no size of its own at all and so opened at its markup
/// minimum — 820x520, which is too short to see a twelve-channel head's map and the movement
/// limits under it without scrolling. Measured from the shot a rig sent on 2026-09-16 asking
/// for "at least as big as the attached screenshot". 1000 wide since the redesign of 2026-09-30,
/// the rule editor's width, as the approved mockup has it.
inline constexpr float kFixturesWindowWidth = 1000.0f;
inline constexpr float kFixturesWindowHeight = 800.0f;
/// The About box, as its approved mockup of 2026-09-30 (`design/weltformat-dark/about1.html`):
/// three sheets and CLOSE, whole at this size.
inline constexpr float kAboutWindowWidth = 580.0f;
inline constexpr float kAboutWindowHeight = 640.0f;

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

/// §5.5's latency offset, in milliseconds either side of zero: `settings::
/// kMaxLatencyOffsetSeconds`, which is also as far as a settings file is taken at its word.
inline constexpr double kLatencyLimitMs = settings::kMaxLatencyOffsetSeconds * 1000.0;

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

/// The outputs heading's "/takt4 global messages to", from the rows: how many are OSC, how many of
/// those are ticked, and what the closed box says — "nothing", the names ticked, or "every OSC
/// output" when two or more are and that is all of them.
void publishGlobalMessages(MainWindow& window, const std::vector<OutputRow>& rows);

/// What the device picker shows. The host API leads, because on Windows one interface
/// appears under both ASIO and WASAPI and the two are not interchangeable (§5.1): only
/// the ASIO entry picks channels natively, and only the WASAPI one offers loopback.
std::string describeDevice(const audio::InputDevice& device);

/// What the channel picker shows, numbered from 1 as it is printed on the interface.
/// ASIO and CoreAudio hand over the driver's own names for them; nothing else does.
std::string describeChannel(const audio::InputDevice& device, int channel);
/// A stereo pair of inputs, `first` and the one after it, as the channel picker lists it:
/// "In 11 + 12", with the driver's names for the two when it has them.
std::string describePair(const audio::InputDevice& device, int first);

/// One decimal place or two, without disturbing any stream's flags.
std::string fixed(double value, int places);

/// The version as the status bar's corner shows it, on two lines: the version, and under it
/// "release" for a release or "dev" for a build between releases — the operator's call of
/// 2026-09-29, "always two lines". A dev build's full label, commit and all, is in the title bar
/// and the About box; see `MainWindow.version-short` for why the corner cannot hold it.
std::string shortVersionLabel(std::string_view version, std::string_view commit);

/// A number of milliseconds as somebody typed it into a reading (`NumberEntry`): "12", "+12",
/// "-30.5", "12 ms", "12ms", with spaces anywhere around it. Nothing for anything else — the
/// caller says so rather than guessing. Not clamped: that is the caller's, against its own limit.
std::optional<double> readMilliseconds(std::string_view text);

} // namespace takt4::ui
