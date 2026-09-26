#pragma once

#include "core/audio/asio_driver.hpp"
#include "core/audio/devices.hpp"
#include "core/audio/input_watchdog.hpp"
#include "core/control/midi_control.hpp"
#include "core/control/osc_control.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/output/link_peers.hpp"
#include "core/output/output_runner.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/tap_tempo.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "ui/fixtures_controller.hpp"
#include "ui/rules_controller.hpp"
#include "ui/window_state.hpp"

#include "main_window.h" // generated from main_window.slint

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace takt4::ui {

/// Everything the window needs that is not the window: the tracker behind it, the model
/// behind its trace, and the one timer that drains the engine's rings.
///
/// Nothing here is ever reached from the audio thread, which is the whole of HANDOFF
/// §7.5's rule. The engine writes into lock-free rings from its own threads; this reads
/// them on the UI thread, on a timer, and touches a Slint property nowhere else.
///
/// It is a header of its own rather than a class hidden in `app.cpp` so that
/// `takt4_ui_tests` can drive it under Slint's testing backend: press the real button,
/// step the real redraw, and read the real properties back. Without that, the wiring
/// between a click and the tracker could only be checked by a person at the machine.
class WindowController {
public:
    /// How often the window drains the engine's rings and redraws. §7.5: "Push audio-side
    /// results through a lock-free ring and drain on a UI timer — do not queue one closure
    /// per audio callback." 30 Hz is slower than the 50 Hz the frames arrive at, so every
    /// tick finds one or two waiting and none is ever missed; the ring holds ten seconds
    /// of them, so even a stalled event loop loses nothing.
    static constexpr std::chrono::milliseconds kRedrawInterval{33};

    /// How far the peak indicator falls back each tick — slow enough that a moment of
    /// clipping is still on screen when the operator looks up.
    static constexpr float kPeakDecay = 0.88f;

    /// Redraws a posted settings change is given to appear on the engine before the
    /// window goes back to showing what the engine actually has. See `postOptions`.
    /// Three redraws is about a tenth of a second, where the engine needs about two
    /// milliseconds — so this is a backstop, not a timing assumption.
    static constexpr int kSettleRedraws = 3;

    /// How often the autosave looks, in redraws — about twice a second. See `enableAutosave`.
    static constexpr std::uint64_t kAutosaveCheckTicks = 15;

    /// The tracker must outlive this. Builds the window, fills the pickers from the
    /// tracker's device list, and starts the redraw timer.
    ///
    /// It also builds §4.2's output thread and hands the tracker Link's clock, which is
    /// why nothing outside has to know the ordering §4.3 needs: `LiveTracker::start`
    /// installs the clock before it opens the stream.
    ///
    /// `settings` is what the last run left behind (Q7): the interface and channel to
    /// select if they are still there, and the outputs to switch back on. The tracker's
    /// own tuning is not applied here — it belongs to the engine, and whoever built the
    /// `LiveTracker` passes it in `Options::engine::tempo`.
    ///
    /// Two constructors rather than a default argument: §6 records that
    /// `const Settings& = {}` in a declaration compiles on MSVC and on nothing else.
    WindowController(engine::LiveTracker& tracker, const settings::Settings& settings);
    explicit WindowController(engine::LiveTracker& tracker);
    /// Stops the tracker and takes Link's clock back off it — the clock is the runner's, and
    /// goes with this. A tracker left running past its window would stamp every hop through a
    /// pointer into a runner that had been destroyed.
    ~WindowController();

    WindowController(const WindowController&) = delete;
    WindowController& operator=(const WindowController&) = delete;

    /// Shows the window and runs Slint's event loop until it is closed.
    void run();

    /// The window itself, for reading properties back.
    MainWindow& window() { return *window_; }
    const slint::ComponentHandle<MainWindow>& handle() const { return window_; }

    /// One round of draining and redrawing — what the timer calls. Public so a test can
    /// step it without waiting on a wall clock, and so that the timer path and the work
    /// it does can be checked separately.
    void tick();

    /// What the window's callbacks do, reachable directly as well as through a click.
    void pickDevice(int index);
    void pickChannel(int index) { channel_ = index; }
    void toggleRun();
    /// START or STOP as the button presses it: says so on the button, lets the window draw
    /// that, and then `toggleRun` — refusing another press until a moment after it is done
    /// (the audit's M23). `toggleRun` is the same thing at once, for a caller that is not a
    /// hand on a button.
    void requestToggleRun();

    /// Looks for input devices and MIDI ports again — the RESCAN button. Only while stopped:
    /// PortAudio renumbers every device when it looks. Keeps the choice that was made (or the
    /// remembered one, if the last launch fell back from it) by name, and tries again anything
    /// remembered that was missing: the MIDI clock port, and any MIDI output a target names.
    void rescanDevices();

    /// Whether the operator has asked for the tracker to run: true from Start to Stop, and
    /// **through an outage the window is recovering from**, when the tracker itself may be
    /// stopped between attempts. It is what the START/STOP button shows.
    bool wantsRunning() const noexcept { return wantRunning_; }

    /// §C4's supervision of a running input, one look — what the redraw timer does each round.
    /// `reading` is the watchdog's and `events` the ASIO driver's, both handed in rather than
    /// read here so a test can deliver a dead input or a driver reset that nothing on this
    /// machine can produce. `now` is on `nowSeconds()`'s clock.
    ///
    /// Silence begins an outage: the readout says NO AUDIO, the status line says which device,
    /// and the input is reopened after a second, then every two, with the device list looked at
    /// again now and then in case it went away and came back under another number. A moved
    /// clock or a driver asking to be reset reopens at once. The outputs are not stopped for
    /// any of it: Link and the MIDI clock carry the last tempo on while the input comes back.
    void superviseInput(const audio::InputWatchdog::Reading& reading,
                        const audio::AsioDriverEvents& events, double now);

    /// For the tests: the state `input` opening and then going silent at `now` leaves — running
    /// as far as the operator is concerned, the tracker stopped, an outage begun. Nothing on a
    /// test machine can be unplugged, and the tests' sandbox opens no device to go silent, so
    /// this is the only way the outage's own path can be driven there (the audit of 2026-09-25,
    /// H1). Every reopen it then tries fails the way a dead interface's does.
    void beginOutageOn(const engine::LiveTracker::Running& input, double now);

    /// §5.5's manual controls. Each one posts on the engine's control queue and returns;
    /// the inference thread applies it before the next frame it tracks, so none of them
    /// blocks a redraw and none of them touches the tracker from this thread.
    void halve();
    void redouble();
    void snapDownbeat();

    /// §5.7's lock, pinned rather than set. Takes the state being asked for rather than
    /// toggling: the button already shows the current one, so sending the intention is
    /// what makes two quick presses land as two changes instead of reading the same
    /// not-yet-updated value twice.
    void setPinned(bool pinned);

    /// One tap. The no-argument form reads this object's steady clock; the other takes
    /// the time, which is how `tracking::TapTempo` is built to be driven and what lets a
    /// test tap out a tempo without spending it in real time.
    void tap();
    void tap(double seconds);

    /// §5.5's settings. Clamped to what the sliders offer, then posted whole — a slider
    /// sends an absolute value, so two arriving in one round cannot lose a step the way
    /// a relative nudge can.
    void setFoldEnabled(bool on);
    void setFoldMin(double bpm);
    void setFoldMax(double bpm);
    void setLatencyMs(double milliseconds);
    /// A number typed into the latency reading, as typed. Read, clamped and posted, and the
    /// slider moved to it; anything that is not a number is said on the status line.
    void setLatencyTyped(const std::string& text);
    /// Whether ÷2 and ×2 carry on into the next track; see `Options::keepOctaveShift`.
    void setKeepShift(bool keep);

    /// The settings as the engine has them — or as it is about to, when a change posted
    /// moments ago has not been applied yet. This is what the window is showing, and it
    /// is what an edit starts from, so that two quick drags do not undo each other.
    tracking::TempoTracker::Options settings() const;

    /// Taps counted so far in the set being tapped in, for the button's own label.
    std::size_t taps() const noexcept { return taps_.taps(); }

    /// The Link output's switch — the tick box on its row, which is always the first. Kept as
    /// a call of its own for the callers that think of Link as a switch.
    void setLinkEnabled(bool on);
    /// SHOW PEERS / HIDE PEERS: the list of Link peers under the Link row, and the listener
    /// behind it (`output::LinkPeerWatch`), which is open only while the list is.
    void toggleLinkPeers();
    bool linkPeersShown() const noexcept { return linkPeersShown_; }

    /// §5.6's targets, one row each — see `OutputRow`.
    ///
    /// A row is two boxes and a switch, and the two boxes are edited a character at a time,
    /// so the two halves are separate: `editTarget` remembers what has been typed and
    /// `acceptTarget` applies it. Applying rebuilds the sockets behind the list, which is
    /// not something to do once per keystroke — but neither is losing a half-typed address
    /// the moment [+] republishes the list, which is what remembering it prevents.
    void editTarget(int index, const std::string& name, const std::string& address);
    void acceptTarget(int index, const std::string& name, const std::string& address);

    /// One field of row `index`, the rest of the row left as it was.
    ///
    /// A row is four fields now rather than two boxes — a name, a kind, and then either a
    /// host and a port or a device — and two of them live inside an `if` in the markup, where
    /// one branch cannot read the other's boxes. So each control reports only itself and the
    /// merge happens here, against the draft this class already owned.
    ///
    /// `apply` is the Enter half: false remembers the keystroke, true rebuilds the sockets.
    /// The two picked-from-a-list ones have no half-typed state, so they always apply.
    void setTargetName(int index, const std::string& name, bool apply);
    void setTargetHost(int index, const std::string& host, bool apply);
    void setTargetPort(int index, const std::string& port, bool apply);
    /// OSC, MIDI, Art-Net or MIDI clock, as an index into `output::OutputTarget::Kind`'s own
    /// order. Never Link, and never the Link row: there is one, and it is always there.
    void setTargetKind(int index, int kind);
    /// Row `index`'s device — a MIDI output's or a MIDI clock's — as an index into the window's
    /// `output-devices`, 0 being that list's "not chosen yet" label, which leaves the row
    /// sending nowhere.
    void setTargetDevice(int index, int device);

    /// A row to fill in, starting from OSC on this machine at the default port. Applied, so
    /// the target exists at once and a rule can be routed to it before it has been aimed.
    void addTarget();
    /// The port a new row starts at: 9000 unless told otherwise, which only a test does — so
    /// that its [+ ADD OUTPUT] sends to a receiver it holds rather than to whatever listens on
    /// this machine's 9000, which on the rig is the operator's own (the audit of 2026-09-25, T1).
    void setNewOutputPort(std::uint16_t port) noexcept { newTargetPort_ = port; }
    /// Any row but the Link row, which is switched off rather than removed.
    void removeTarget(int index);
    void setTargetEnabled(int index, bool on);
    /// Row `index`'s per-output delay, in milliseconds — §5.6's answer to a rig whose
    /// destinations do not all have the same lag. See `output::OutputTarget::delaySeconds`.
    void setTargetDelay(int index, float ms);
    /// A number typed into row `index`'s delay reading, as typed — `setTargetDelay` once read;
    /// anything that is not a number is said on the status line and changes nothing.
    void setTargetDelayTyped(int index, const std::string& text);

    /// Every target from one piece of text — `name = host:port` or `name = midi Device`,
    /// separated by commas or newlines — replacing the whole list.
    ///
    /// What the outputs row was before it was a list, kept because it is still the shape a
    /// settings file's line has and the shape a rig gets pasted in. A part that will not
    /// parse is reported on the status line and the rest are still applied. The Link row
    /// stays, first, as it was.
    void setOscTargets(const std::string& text);

    /// §5.7's control input. The port is opened at once rather than at Start: an operator
    /// binding buttons is doing it *before* the set, and a learn mode that needs the
    /// tracker running would be a worse tool than a pen and paper.
    void setMidiControlPort(const std::string& name);
    /// The same, as an index into `midi-in-ports` — 0 being its "select input" entry.
    void pickMidiControlPort(int index);

    /// §5.7's *other* control input: the OSC listening socket, so a Stream Deck or Bitfocus
    /// Companion can drive this without touching the laptop.
    ///
    /// Off until asked, like the MIDI port and for a stronger reason — a listening socket is
    /// something to open on somebody's say-so, never on their behalf. `port` 0 asks the
    /// platform for a free one; `oscControlPort()` reports which it got, which is what an
    /// operator has to point their surface at.
    void setOscControlEnabled(bool on);
    void setOscControlPort(int port);
    /// Whether anything but 127.0.0.1 is accepted. False is the default; true is what a
    /// control surface on another machine needs, and is the operator's call because OSC
    /// carries no authentication and takt4 invents none.
    void setOscControlNetwork(bool allowNetwork);

    /// The OSC control surface, for reading and for a test to `dispatch` into.
    control::OscControl& oscControl() noexcept { return oscControl_; }
    const control::OscControl& oscControl() const noexcept { return oscControl_; }
    /// The port actually listening, or 0 when nothing is. Not the port that was *asked*
    /// for: with port 0 those differ, and the operator needs the real one.
    std::uint16_t oscControlPort() const noexcept { return oscControl_.port(); }
    /// Which action LEARN will bind, as an index into `control::kControlActions`.
    void pickLearnAction(int index);
    /// Arms learn mode for that action, or disarms when it is already armed. One button
    /// for both, so an operator who pressed it by mistake is not stranded in a mode.
    void toggleLearn();
    /// Unbinds every control bound to the selected action.
    void forgetLearned();

    /// §5.8's rules, as this window has them.
    ///
    /// **The window's copy is the editable one and the runner's is the live one.** They are
    /// separate on purpose: the rules belong to §4.2's output thread, so reading them back
    /// through `outputs().triggers()` is only safe while it is stopped — and an editor has
    /// to work during a set. Everything here edits this copy and posts the set whole, which
    /// is the same shape `postOptions` uses for a slider and what `OutputCommand::Rules`
    /// was written for.
    const std::vector<trigger::Rule::Config>& rules() const noexcept { return rules_; }
    /// Replaces the set, hands it to the output thread, and reports what will not fire.
    void setRules(std::vector<trigger::Rule::Config> rules);

    /// §5.9's rule editor, in its own window. Built with this one and shown on demand: an
    /// operator who never writes a rule never sees it, and one who does keeps their place
    /// in it across opening and closing.
    RulesController& editor() noexcept { return editor_; }
    /// The lighting patch editor, for `takt4_ui_tests` to drive the way it drives the rule
    /// editor — Slint's element-finding API being behind SLINT_FEATURE_EXPERIMENTAL (§6).
    FixturesController& patchEditor() noexcept { return patch_; }
    /// Opens the editor, or brings it forward.
    void openEditor();
    /// Opens the About box, or brings it forward.
    void openAbout();
    /// The About box once it has been opened, for the tests; null before.
    AboutWindow* about() noexcept { return about_ ? &**about_ : nullptr; }
    /// What opening a written-out licence does. The default hands it to the machine's viewer;
    /// takt4_ui_tests replaces it, since a test must not launch one. False when it could not.
    void setFileOpener(std::function<bool(const std::filesystem::path&)> opener) {
        openFile_ = std::move(opener);
    }
    /// Writes `text` to `name` under the temp directory and opens it: what the About box's two
    /// buttons do, the licence and the notices being built in. The file written, or empty.
    std::filesystem::path openEmbeddedText(const std::string& name, std::string_view text);
    /// §5.8's PANIC from the main window's own row, so a halt never waits on a window.
    ///
    /// **Engage only, and release is separate** — the audit's H18. This was `togglePanic`, so a
    /// double-click on PANIC, which is how a button gets hit in a hurry, halted the rig and let
    /// it go again before anybody saw it light. Pressing PANIC while panicked does nothing now;
    /// RELEASE, which appears beside it, is the only way back.
    void engagePanic();
    void releasePanic();

    /// The MIDI surface itself. Non-const for the same reason `window()` is: a test drives
    /// `dispatch` to deliver an event no hardware here can send, which is the only way the
    /// path from a controller to a binding is checkable without somebody pressing a pad.
    control::MidiControl& control() noexcept { return control_; }
    const control::MidiControl& control() const noexcept { return control_; }
    /// Every MIDI *input* port on the machine, as offered in the picker.
    const std::vector<std::string>& midiInputPorts() const noexcept { return midiInputPorts_; }

    /// Everything worth remembering for next time (Q7), as it stands now. The caller
    /// saves it; this class does not know where settings live and does not want to.
    settings::Settings currentSettings() const;

    /// Write `currentSettings()` to the usual place, now, rather than on the way out.
    ///
    /// **Why this exists.** `app.cpp` saves when the window closes and nowhere else, on the
    /// reasoning that a slider drag would otherwise write the file sixty times a second —
    /// which is right, and leaves an operator who has spent an hour building rules with no
    /// way to commit them and nothing to do but trust a clean exit. They said so on
    /// 2026-09-12: *"I am scared of closing it"*. A button is the answer; the automatic
    /// save on exit stays exactly as it was.
    bool saveNow();

    /// The same bytes to a file of the operator's choosing, for a backup or a second rig.
    bool exportTo(const std::filesystem::path& path);

    /// Writes the settings to `file` on its own, `quietSeconds` after the last change.
    ///
    /// **Because a crash, a power cut or Windows shutting down used to lose the whole
    /// session.** The file was written on a clean exit and on SAVE and at no other time, so a
    /// rig built that afternoon lived only in memory until somebody remembered the button. The
    /// audit's C8. This checks about twice a second whether what would be written has changed,
    /// and writes once it has stopped changing — so a slider drag is one save when it ends, not
    /// sixty while it moves, and the file is never more than a few seconds behind the window.
    ///
    /// Off until called. `ui::run` switches it on for the real settings file, and nothing else
    /// does, so a test never writes beside its own executable.
    void enableAutosave(std::filesystem::path file, double quietSeconds = 3.0);

    /// A sentence the operator must see — `settings::Startup::notice`, most often: the file was
    /// damaged and what was done about it. On the status line, as an error.
    void showNotice(const std::string& text);

    /// Load a file and apply **the portable half** — Q7's preset: the rules, the outputs,
    /// the fold window and the rest of `TempoTracker::Options`, the meters, Link and the OSC
    /// prefix. The machine-local half is deliberately left alone: the audio device, the MIDI
    /// clock port and the learned bindings describe *this* desk, and a preset carried from
    /// another one naming a device that is not here would silently stop the tracker.
    ///
    /// False only when the file could not be read as settings at all. Note that
    /// `settings::load` never fails, so this reports on the file existing and parsing rather
    /// than on the contents being sensible.
    bool importFrom(const std::filesystem::path& path);

    /// What is being sent, for the row that draws it. The runner is running for as long as
    /// this window exists, so its rules and transports are read through `inspect` — see there.
    const output::OutputRunner& outputs() const noexcept { return runner_; }
    /// Waits until the output thread has taken every change posted to it so far. What a test
    /// calls between a gesture and reading what it did.
    bool settleOutputs() { return runner_.sync(); }
    /// Every MIDI output port on the machine, as offered in the picker.
    const std::vector<std::string>& midiPorts() const noexcept { return midiPorts_; }

    const std::vector<audio::InputDevice>& devices() const noexcept { return devices_; }
    /// Index into `devices()`, or -1 when the machine has none.
    int deviceIndex() const noexcept { return device_; }
    /// 0-based channel of the selected device.
    int channelIndex() const noexcept { return channel_; }
    bool statusIsError() const noexcept { return statusIsError_; }

    /// Times `tick()` has been entered, however it was reached. What a test watches to
    /// tell "the redraw timer is running" from "the redraw does the right thing" — two
    /// separate claims that a window either meets or silently does not.
    std::uint64_t ticks() const noexcept { return ticks_; }

private:
    /// Fills the device picker and selects one: the remembered device if it is still
    /// there, otherwise the most useful one on the machine.
    void refreshDevices(const settings::MachineSettings& remembered);
    /// Reads the tracker's device list into `devices_` and the picker's model, selecting
    /// nothing.
    void listDevices();
    /// After an outage's rescan: the list again, and the input the outage is about found in it
    /// by name — or, when it is not there, nothing selected in its place and it remembered as
    /// the one wanted (the audit of 2026-09-25, H1).
    void relistDuringOutage();
    /// The MIDI pickers' models, from `midiPorts_` and `midiInputPorts_`.
    void publishPortLists();
    /// Closes the input and opens the same device and channel again, found by name — a
    /// rescan renumbers devices. False, with `error` saying why, when it would not open.
    bool reopenInput(std::string& error);
    /// A reopen for `why`, now; an outage when it fails.
    void restartInput(const std::string& why, double now);
    void beginOutage(const std::string& why, double since, double now);
    /// Which MIDI devices have gone quiet, from the runner's snapshot, said on the status line
    /// when the set changes. "Back" only of a device one of `outputs` still sends to.
    void publishLostMidi(const std::vector<std::string>& lost,
                         const std::vector<output::OutputTarget>& outputs);
    /// The same for outputs that cannot be sent to — a host name that will not resolve, found
    /// out on a thread of its own long after the edit that typed it was applied.
    void publishOutputProblems(const std::vector<std::string>& problems);
    /// The output thread's counters, from the runner's snapshot, on the line under the outputs
    /// heading — and a stage that threw on the status line, when what it said is new.
    void publishOutputTrouble(const output::OutputRunner::Snapshot::Trouble& trouble);
    void publishStopped();
    void publishOpenStream();
    void publishOptions();
    void publishPin();
    void publishTrace();
    void publishState();
    void publishLevels();
    void publishTaps();
    void publishOutputs();
    /// The Link peers list, while it is shown: what the listener has heard since the last
    /// round, into the rows under the Link row.
    void publishLinkPeers();
    /// The drafts, parsed into targets and handed to the output thread. Rows that will not
    /// parse are kept as they were typed and named on the status line; a row whose address
    /// holds several targets — a pasted line — becomes several rows.
    void applyTargets();
    /// The drafts into the window's model. Never called from `editTarget`: replacing a row
    /// re-evaluates the `text:` binding of the box being typed into.
    void publishTargetRows();
    /// Both of §5.7's surfaces. Each half publishes independently, because the MIDI half
    /// returns early when no port is open and anything written after that return would
    /// never run on a window that has only the OSC socket.
    void publishTriggers();
    /// `force` writes the OSC control port back into its field whatever it currently holds.
    /// True for every caller that is an operator doing something, and **false from the
    /// redraw timer**, whose job is to show what changed rather than to overwrite the box
    /// somebody is typing a port into. See `publishOscControl`.
    void publishControl(bool force = true);
    void publishMidiControl();
    void publishOscControl(bool force);
    void publishSnap();
    /// One redraw round from inside the platform's own window-drag loop, where Slint's timer
    /// does not run — `ui::keepPaintingWhileDragged`, which this is registered with. Static
    /// because it is reached through a C callback; `self` is the controller.
    static void pumpWhileDragged(void* self);
    /// Writes this round's number to `tickProbe_` and sweeps the input meter — see that
    /// member. Called only when the environment asked for it.
    void writeTickProbe();
    /// Sends a whole `Options` and remembers it until the engine is seen to have it.
    void postOptions(const tracking::TempoTracker::Options& options);
    /// One look at whether the settings need writing — see `enableAutosave`. `now` is
    /// `nowSeconds()`.
    void autosave(double now);
    void setStatus(const std::string& text, bool error);
    /// Seconds since this controller was built, on a steady clock. Only differences are
    /// used, which is all `tracking::TapTempo` asks of it.
    double nowSeconds() const;

    /// The constructor proper, handed settings that have been through `settings::assignIds` —
    /// the public one does that first, so that the runner, both editors and the output rows
    /// are all built from one set of ids rather than each generating its own.
    struct IdsAssigned {};
    WindowController(engine::LiveTracker& tracker, const settings::Settings& settings, IdsAssigned);

    engine::LiveTracker& tracker_;
    std::vector<std::string> midiPorts_;
    std::vector<std::string> midiInputPorts_;
    slint::ComponentHandle<MainWindow> window_;
    slint::Timer timer_;

    std::vector<audio::InputDevice> devices_;
    int device_ = -1;
    int channel_ = 0;

    /// The machine half as the last run left it, kept for the whole session — see
    /// `deviceFallback_` for why a fallback must not overwrite it.
    settings::MachineSettings remembered_;
    /// The remembered interface was not on the machine when the list was read, so another was
    /// selected. **Saving that one as if it had been chosen** meant a launch before the MOTU
    /// was powered on made the next show open on the wrong device at channel 1 (the audit's
    /// H11). While this holds and the operator has not picked anything, the remembered one is
    /// what gets saved.
    bool deviceFallback_ = false;
    /// The operator picked a device or a channel from the pickers this session.
    bool deviceChosen_ = false;
    /// OSC control asked for — by the settings or by its switch — whether or not its port
    /// bound. Saved as it is, so a port that was taken at one launch is still wanted at the
    /// next (the audit's M24).
    bool oscControlWanted_ = false;
    /// True until the constructor has finished, and the errors it met meanwhile — shown
    /// together at the end of it rather than each writing the last away (the audit's M25).
    bool constructing_ = true;
    std::vector<std::string> startupErrors_;
    /// The lost MIDI devices the status line last reported.
    std::vector<std::string> lostMidiShown_;
    /// See `setNewOutputPort`.
    std::uint16_t newTargetPort_ = 9000;
    /// The output problems the status line last reported. See `publishOutputProblems`.
    std::vector<std::string> outputProblemsShown_;
    /// The output thread's counters as the window last showed them. See `publishOutputTrouble`.
    output::OutputRunner::Snapshot::Trouble outputTroubleShown_;
    /// The runner's snapshot as `publishOutputs` last copied it, and which one that was.
    output::OutputRunner::Snapshot snapshot_;
    std::uint64_t snapshotShown_ = 0;

    /// §C4's watch on the running input. See `superviseInput`.
    audio::InputWatchdog watchdog_;
    bool wantRunning_ = false;
    /// What was opened, for reopening the same device and channel by name.
    std::optional<engine::LiveTracker::Running> input_;
    /// An outage in progress: since when, when to try again, how many tries so far.
    struct Outage {
        double since = 0.0;
        double nextTry = 0.0;
        int tries = 0;
        std::string why;
    };
    std::optional<Outage> outage_;
    /// When PortAudio last looked for devices during an outage; negative before that.
    double rescannedAt_ = -1.0;
    /// See `requestToggleRun`. A member, so a press still waiting to be carried out goes with
    /// the window rather than calling into it after it has gone.
    slint::Timer runTimer_;
    bool runPending_ = false;
    /// When the last press finished being carried out; negative before any.
    double runSettledAt_ = -1.0;
    /// What went missing between the input and the beats, as the window last showed it: the
    /// interface's overflows and the engine's drops, all of which start again with every run.
    struct InputTrouble {
        std::uint64_t overflows = 0;
        std::uint64_t hopsDropped = 0;
        std::uint64_t framesDropped = 0;
        std::uint64_t beatsDropped = 0;
        std::uint64_t samplesRepaired = 0;
        bool operator==(const InputTrouble&) const = default;
    };
    InputTrouble inputTroubleShown_;

    tracking::TapTempo taps_;
    /// When the last tap landed, so a set that has gone quiet stops claiming to be
    /// counting. `TapTempo` only notices its own timeout on the *next* tap.
    double lastTapSeconds_ = 0.0;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    /// The settings last posted, until the engine is seen to have them or `kSettleRedraws`
    /// have passed. While one is in flight the window shows it rather than what the engine
    /// still has: a redraw landing in the couple of milliseconds before the inference
    /// thread applies a change would otherwise snap the slider back out from under the
    /// operator's finger.
    std::optional<tracking::TempoTracker::Options> posted_;
    int settling_ = 0;

    /// The same thing for the pin, which travels as its own command rather than inside
    /// the settings — a pin followed by a release is not a pin, so it must not be
    /// superseded the way `SetTempoOptions` is.
    std::optional<bool> pinPosted_;
    int pinSettling_ = 0;

    /// The trace as a plain buffer, oldest first, mirrored into the model each tick.
    std::vector<TracePoint> trace_;
    std::shared_ptr<slint::VectorModel<TracePoint>> traceModel_;
    /// A beat that landed on a frame the engine interpolated, waiting for the next of the
    /// network's frames to be drawn on. See `EngineFrame::interpolated`.
    bool traceBeatPending_ = false;

    /// §5.6's targets **as they are being typed**, which is not the same list as the one
    /// behind `Transports`: a row half-way through an address is not a target yet, and an
    /// empty row is a place to type rather than an output that sends nowhere. This is the
    /// window's copy and the one the rows are drawn from; `applyTargets` is what turns it
    /// into the transports' list.
    std::vector<OutputRow> targetDrafts_;
    /// What each output row's boxes show **because somebody typed or picked it there**, since
    /// that row's element was built — by element, which is by index. A box typed into has lost
    /// its `text:` binding (a dropdown picked from, its `current-index:`), so it shows this and
    /// not the model; a box nobody has touched follows the model on its own.
    struct ShownRow {
        std::optional<std::string> name;
        std::optional<std::string> host;
        std::optional<std::string> port;
        std::optional<int> kind;
        std::optional<int> device;
    };
    std::vector<ShownRow> shownRows_;
    ShownRow& shownRow(int index);
    /// Rows `publishTargetRows` found showing something other than the row they now hold — a
    /// value the controller normalised after an edit, or a delete that moved every row below
    /// it up one — consumed by `tick`, which builds each **one** again (`renewRows`) so its boxes
    /// come back bound. Deferred rather than done there and then, because the publisher runs
    /// inside the callback of the row being destroyed.
    std::vector<std::size_t> staleTargetRows_;
    /// The last thing the output thread said went wrong, as this window has already shown it.
    /// Watched in `tick` because `post` is asynchronous while the tracker runs — see the note
    /// there, which is a bug report about MIDI ports that failed to open in silence.
    std::string outputErrorShown_;
    std::shared_ptr<slint::VectorModel<OutputRow>> targetModel_;

    /// SHOW PEERS: whether the list is open, the listener behind it, and its rows. The
    /// listener joins Link's multicast group only while the list is shown.
    bool linkPeersShown_ = false;
    output::LinkPeerWatch linkPeers_;
    std::shared_ptr<slint::VectorModel<LinkPeer>> peerModel_;

    /// `enableAutosave`'s state. Empty `autosaveFile_` is autosave off.
    std::filesystem::path autosaveFile_;
    double autosaveQuietSeconds_ = 3.0;
    /// What the file holds, as far as this window knows — the text of the last save it made,
    /// or of the settings it started from. A change is this differing from what would be
    /// written now.
    std::string savedText_;
    /// A change seen but not yet written, and when it was first seen in this exact form. The
    /// text moving again restarts the wait, which is what makes a drag one save.
    std::string pendingText_;
    double pendingSince_ = 0.0;
    /// When a save last failed, or negative. A full disk is retried now and then rather than
    /// twice a second, and said once rather than on every attempt.
    double autosaveFailedAt_ = -1.0;
    float peak_ = 0.0f;
    bool statusIsError_ = false;
    std::uint64_t ticks_ = 0;

    /// Where to write this window's redraw count each round, from the `TAKT4_TICK_PROBE`
    /// environment variable, or empty — which is every ordinary run.
    ///
    /// **A bench switch that ships**, and that is the point of it being read at run time
    /// rather than compiled in: the thing it measures is whether the window is still being
    /// driven and still painting from inside a platform modal loop — a drag, a resize — and
    /// nothing that can be watched from outside the process survives that loop. Slint's timer
    /// stops; a Slint property is only pushed to the platform on the update pass that stops
    /// with it. A file written from `tick()` and a swept input meter do not, so together they
    /// say whether the round happened *and* whether anything reached the screen.
    ///
    /// With it compiled in behind a build flag the test would have measured a binary nobody
    /// runs. `scripts` for it are not vendored; the shape is: start takt4 with the variable
    /// set, put the window into the move loop with `WM_SYSCOMMAND`/`SC_MOVE`, and compare
    /// screenshots a second apart. See the drag pump in `ui::keepPaintingWhileDragged`.
    std::string tickProbe_;

    /// The OSC control port as this controller last wrote it into the field. The field is
    /// bound two-way, so it is also whatever an operator has typed since — which is why the
    /// redraw compares against *this* and not against the widget: the two differing is what
    /// being typed into looks like. See `publishOscControl`.
    std::string oscControlPortShown_;

    /// `settings::Preset::meters` as this window was built with them. The engine was built
    /// with the same list (`app.cpp`) and nothing in the window changes it yet, so this is
    /// the value `currentSettings` writes back — kept here rather than read from the
    /// decoder because the decoder interface does not expose its bars.
    std::array<std::uint8_t, 4> meters_{4, 0, 0, 0};

    /// The tracker's beat count when a downbeat snap was posted, while one is still in
    /// flight. §5.5's snap lands on the *next* beat called, so the count moving is what
    /// says it has arrived — and until then the DOWNBEAT button stays lit, because a
    /// control that does nothing visible for the best part of a second reads as broken.
    std::optional<std::uint64_t> snapAwaitingBeat_;

    /// §5.8's rules as this window has them; see `rules()`.
    std::vector<trigger::Rule::Config> rules_;
    /// How many of them are active, counted whenever they change. See `publishTriggers`.
    int rulesActive_ = 0;
    /// The lighting patch, as this class has it for saving. `patch_` owns the editing; this is
    /// the copy that goes into a settings file, kept in step by its changed callback.
    std::vector<dmx::Fixture> fixtures_;
    /// A decoder and an OSC prefix an imported preset asked for that this run cannot switch to
    /// — both are fixed while the application runs — saved so the next launch uses them.
    std::optional<tracking::Decoder> pendingDecoder_;
    std::optional<std::string> pendingPrefix_;

    /// §5.7's control input. Which action LEARN would bind, as an index into
    /// `learnActions_`; the binding table itself lives in `control_`.
    int learnAction_ = 0;
    /// The actions this window offers for binding: `control::kControlActions` minus the
    /// ones that need a rule named too, which a gesture cannot supply. See the constructor.
    std::vector<control::ControlAction> learnActions_;

    /// §4.2's output thread, and the single consumer of the engine's beat ring — which is
    /// why `tick()` no longer drains it.
    ///
    /// Its destructor joins the thread, and that has to happen before anything the thread
    /// could still be touching goes away. Nothing it holds today reaches back into this
    /// class, but a beat observer is the obvious next thing to give it.
    output::OutputRunner runner_;

    /// §5.7's two control surfaces. **Both after `runner_`, so both are destroyed first**,
    /// and that ordering is load-bearing rather than a preference: since §5.7's `panic` and
    /// `rule/<id>/enable` landed, each of these posts to the *runner* as well as to the
    /// engine — MIDI on RtMidi's callback thread, OSC on its own receive loop. Destroying
    /// the runner first would leave either thread with a queue that has gone. The engine
    /// outlives all three either way, which is what made the opposite order safe before and
    /// is no longer the whole question.
    ///
    /// Two surfaces rather than one because each owns its own tap set: a Stream Deck's tap
    /// button and a MIDI pad are two surfaces, and interleaving their taps would give an
    /// operator using both a tempo neither meant (`control::ControlSurface`).
    control::MidiControl control_;
    control::OscControl oscControl_;

    /// §5.9's editor. **After the two surfaces**, so it is destroyed before them and long
    /// before `runner_` — it posts to the runner like they do, though only from this thread.
    RulesController editor_;
    /// The lighting patch editor, built with the window and shown on demand — the same shape
    /// as `editor_`, and for the same reason: a patch is set up once and then not touched,
    /// while the rules are edited during the set.
    FixturesController patch_;
    /// The About box, built the first time ABOUT is pressed.
    std::optional<slint::ComponentHandle<AboutWindow>> about_;
    std::function<bool(const std::filesystem::path&)> openFile_;
};

} // namespace takt4::ui
