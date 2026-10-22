#pragma once

#include "core/audio/devices.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tempo_tracker.hpp"

#include "main_window.h" // generated from main_window.slint

#include <cstddef>
#include <cstdint>
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
/// 800 wide since the Weltformat-dark redesign (the design, 2026-09-29): the operator cut the
/// width by a fifth, and every row of the outputs table is laid out to read whole at it.
inline constexpr float kMainWindowWidth = 800.0f;
/// It opens at this with a scroll bar rather than sized to its content (1146 px with five
/// outputs and their peers): the design kept today's height, which opens whole on a 1080-line screen
/// under a title bar and a taskbar.
inline constexpr float kMainWindowHeight = 934.0f;
/// The markup's `min-height` — what folding a section never shrinks the window below.
inline constexpr float kMainWindowMinHeight = 420.0f;
/// The rule editor as the operator locked it on 2026-09-30: 1000 wide — the log
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
/// The About box, as its approved mockup of 2026-09-30 (`the approved design`):
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

/// the latency offset, in milliseconds either side of zero: `settings::
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

/// the settings. Read these from `BeatEngine::tempoOptions()` every time and never
/// from a copy: a tap moves the octave-fold window (the design deviation 8). While a source is followed
/// the latency slider is that source's own (`followedLatency`, in seconds), not the tracker's.
void publishTempoOptions(MainWindow& window, const tracking::TempoTracker::Options& options,
                         std::optional<double> followedLatency = std::nullopt);

/// **Where the beats come from, as the window says it** — the source, what it is saying, what
/// the operator has to do about it, and where in its bar it is. `describeTiming` works it out from
/// the source's own readings and `publishTimingView` puts it on the window; the live window and
/// `takt4-shot` both go through the two, so a picture cannot say what the window would not.
struct TimingFacts {
    /// 0 beat detection, 1 Ableton Link, 2 MIDI clock, 3 the CDJs — `timing-sources`' order.
    int index = 0;
    /// Started: the follower running.
    bool running = false;
    tracking::TempoState state;
    bool coasting = false;
    /// The settings on the rows under the picker, and whether START has what it needs: a MIDI
    /// clock its port.
    std::uint32_t beatsPerBar = 4;
    bool obey = false;
    bool ready = true;
    /// Running: the beats to the bar the run counts in, fixed at its START — 0 while stopped. An
    /// import while running can leave the setting above different from it.
    std::uint32_t runBeatsPerBar = 0;
    /// Link, stopped: what its discovery traffic says — whether it can be heard at all, how many
    /// peers there are, and the tempo the first announces.
    bool canListen = true;
    std::size_t peersHeard = 0;
    double heardBpm = 0.0;
    /// Link, running: peers in the session.
    std::uint32_t peers = 0;
    /// MIDI clock: its port, by name, and what its ticks are doing (`MidiClockFollower::Status`) —
    /// and whether the device it comes in on has been pulled out (`midi::MidiInputHub::isLost`).
    std::string port;
    bool ticking = false;
    bool placed = false;
    bool stopObeyed = false;
    std::uint32_t tickInBeat = 0;
    bool portLost = false;
    /// The CDJs: the network as `timing::prolink::Network::snapshot` has it — why it cannot be
    /// listened to or joined, the players and mixers heard, whether takt4 has joined and as which
    /// player, the master, whom it follows and whom it was asked to (0 the master), whether the
    /// master's track is one that sends no beats, and the card joined on.
    std::string cdjProblem;
    std::size_t cdjPlayers = 0;
    std::size_t cdjMixers = 0;
    bool cdjJoined = false;
    std::uint8_t cdjNumber = 0;
    std::uint8_t cdjMaster = 0;
    std::uint8_t cdjFollowed = 0;
    std::uint8_t cdjFollow = 0;
    bool cdjMasterWithoutBeats = false;
    std::string cdjInterface;
};

struct TimingView {
    int index = 0;
    /// "Ableton Link", "MIDI clock" — empty for beat detection — and short, where it labels
    /// something else: "Link", "MIDI".
    std::string name;
    std::string shortName;
    /// The timing row's reading; the tempo sheet's line under "source"; what to do, in amber.
    std::string reading;
    std::string detail;
    std::string note;
    bool waiting = false;
    /// The tempo sheet's word for waiting: "no peers", "no clock", "no master", "joining".
    std::string waitingWord;
    bool coasting = false;
    int stripBeat = 0;
    int stripTicks = 0;
    /// The strip's words at its right: "beat 2 of 4", "player 2 · 3 of 4".
    std::string stripLabel;
    /// The CDJs' network card, or what picks it.
    std::string network;
    int beatsPerBar = 4;
    bool obey = false;
    bool ready = true;
};

TimingView describeTiming(const TimingFacts& facts);
void publishTimingView(MainWindow& window, const TimingView& view);

/// The MIDI clock's input list, as the control input's is: "select input" first, this machine's
/// inputs, and `wanted` after them, said not to be plugged in, when it is not one of them — found
/// by the rule a port is found by (`output::findMidiPort`). `wanted` is picked.
void publishClockPorts(MainWindow& window, const std::vector<std::string>& inputs,
                       const std::string& wanted);

/// One player the CDJs' "follow" list can name.
struct CdjPlayer {
    std::uint8_t number = 0;
    std::string name;
};

/// The CDJs' "follow" list: "tempo master" first, then each player heard, by number — and the one
/// asked for, when it is not among them, said not to be heard. `follow` (0 the master) is picked.
/// Returns the player numbers in list order, the master's entry as 0, for the controller to
/// turn a pick back into a player.
std::vector<std::uint8_t>
publishCdjFollow(MainWindow& window, const std::vector<CdjPlayer>& players, std::uint8_t follow);

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
/// appears under both ASIO and WASAPI and the two are not interchangeable: only
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
