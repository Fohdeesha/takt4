#pragma once

#include "core/dmx/fixture.hpp"
#include "core/output/output_runner.hpp"
#include "core/settings/settings.hpp"
#include "ui/delete_guard.hpp"

#include "main_window.h" // generated; holds FixturesWindow too — see src/ui/CMakeLists.txt

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace takt4::ui {

/// The lighting patch editor, behind its own window.
///
/// **It edits its own copy of the patch and posts it whole**, which is the same shape
/// `RulesController` takes and for the same reason: the live patch belongs to §4.2's output
/// thread — `DmxEngine` is documented as that thread's alone — and an editor has to work
/// during a set. Every edit changes `fixtures_` and hands the whole patch over through
/// `OutputCommand::Patch`.
///
/// Replacing the patch is cheap and, more to the point, *safe*: levels survive a re-patch where
/// they can (`DmxEngine::setPatch`). And nothing is re-patched per keystroke: typing 101 into the
/// address used to patch the fixture at 1, then 10, then 101, parking each channel it passed over
/// (the audit of 2026-09-25, M22), and a name re-patched the rig on every letter (H8). Every box
/// commits on Enter, on a click away, or before any other action (`commitDrafts`) — and a number
/// box's Up and Down, which are a step each and commit it, as a spin box's arrows would. Nothing is
/// re-patched that has not changed: a re-patch ends a channel's TEST.
///
/// **Every row is written in place** (`writeRows`): the window's controls are weltformat.slint's,
/// none of which sets its own value, so no row has to be built again to show what the model
/// holds — and no element is destroyed under the hand that is using it.
///
/// Nothing here touches a Slint property outside `tick()` and the callbacks, both of which run
/// on the UI thread — §7.5's rule.
class FixturesController {
public:
    /// Called whenever the patch changes, so the owner can save it (Q7) and keep its own copy
    /// in step. The controller does not know where settings live and does not want to.
    using PatchChanged = std::function<void(const std::vector<dmx::Fixture>&)>;

    /// The runner must outlive this. Nothing is shown until `show()`.
    FixturesController(output::OutputRunner& runner, std::vector<dmx::Fixture> fixtures);

    FixturesController(const FixturesController&) = delete;
    FixturesController& operator=(const FixturesController&) = delete;

    void setPatchChanged(PatchChanged changed) { changed_ = std::move(changed); }

    void show();
    void hide();
    bool visible() const noexcept { return visible_; }

    /// One round of refreshing what the window shows — the live levels.
    /// Driven by the main window's redraw timer so the app has one timer.
    ///
    /// **The live levels are the reason this ticks at all.** Everything else in this window is
    /// static until somebody types; the bar beside each channel is what turns a patch editor
    /// into something an operator can confirm a fade with, without a fixture plugged in.
    void tick();

    FixturesWindow& window() { return *window_; }

    const std::vector<dmx::Fixture>& fixtures() const noexcept { return fixtures_; }
    /// Replaces the patch from outside — a preset load. Keeps the selection where it can.
    void setFixtures(std::vector<dmx::Fixture> fixtures);

    /// What the outputs row should say about the lighting rig: how many fixtures, how many
    /// universes, and whether anything is carrying them. Empty when nothing is patched.
    std::string summary() const;

    int selected() const noexcept { return selected_; }

    /// The sheets' folds, as the machine settings remember them (with the rule editor's).
    void applyLayout(const settings::MachineSettings& machine);
    void layoutInto(settings::MachineSettings& machine) const;
    /// 0 A where, 1 B channels, 2 C how far it moves.
    void fold(int section);

    // What the window's callbacks do, reachable directly as well as through a click — which is
    // how `takt4_ui_tests` drives them, Slint's element-finding API being behind
    // SLINT_FEATURE_EXPERIMENTAL (§6).
    void pick(int index);
    void add();
    void remove();
    void removeAt(int index);
    void duplicate();
    /// The copy mark on row `index`: that fixture, copied, the copy placed after it and picked.
    void duplicateAt(int index);
    void setEnabledAt(int index, bool on);

    void rename(const std::string& name);
    void setGroup(const std::string& group);
    void setUniverse(const std::string& text);
    void setAddress(int address);
    void setEnabled(bool on);
    /// Index 0 is "custom" and changes nothing; the rest index `dmx::builtinModes()`.
    ///
    /// **It replaces the channel map and the parked levels and nothing else** — the name, the
    /// group, the universe, the address and the movement window are the operator's and are
    /// kept. Picking a mode is "this fixture is shaped like that", not "start again".
    void pickMode(int mode);
    void addChannel();
    void removeChannel(int index);
    void pickChannelRole(int index, int role);
    void setChannelParked(int index, int level);
    void setPanRange(float low, float high);
    void setTiltRange(float low, float high);
    /// Flashes every light-emitting channel of the selected fixture to full and back to where
    /// it was over `kIdentifySeconds`, so an operator in the truss can see which lamp they are
    /// patching and the lamp is left as it was.
    void identify();

    /// Holds one channel of the selected fixture at `testLevel()` for `kTestSeconds`, then
    /// puts it back — `index` into the selected fixture's channel map.
    ///
    /// **The question IDENTIFY cannot answer.** IDENTIFY flashes the whole fixture, which
    /// says which lamp on the truss this row is; it says nothing about whether channel 72 is
    /// really the blue one, and a channel map that is one out is wrong in exactly that way.
    void testChannel(int index);
    void setTestLevel(int level);
    int testLevel() const noexcept { return testLevel_; }

    /// How long IDENTIFY holds the fixture at full. Long enough to look up and find it,
    /// short enough that walking away does not leave a lamp on.
    static constexpr double kIdentifySeconds = 3.0;
    /// And how long TEST holds one channel. The same reasoning, and the same number so that
    /// the two controls feel like one thing.
    static constexpr double kTestSeconds = 3.0;

private:
    dmx::Fixture* current() noexcept;
    const dmx::Fixture* current() const noexcept;

    /// Hands the patch to the output thread and tells the owner. Every edit ends here.
    void commit();
    /// Applies what is being typed — in a text box or a number box — to the fixture it was typed
    /// for. Called before **every** action — see the constructor, where each callback is wired.
    void commitDrafts();

    /// The three number boxes, whose keystrokes are kept until they are entered — see `typed_`.
    enum class Numbered : std::uint8_t { Address, TestLevel, Parked };
    /// And the three text boxes — see `draft_`.
    enum class Field : std::uint8_t { Name, Group, Universe };
    /// A keystroke in a number box: what Enter would set there now.
    void noteTyped(Numbered box, int index, int value);
    /// A number box's own commit, which counts only if it was typed in for the fixture showing —
    /// see `typed_`. Returns whether it did, having cleared it.
    bool takeTyped(Numbered box, int index);
    /// A keystroke in a text box, and that box's own commit — the same rule.
    void noteDraft(Field field, const std::string& text);
    void finishDraft(Field field, const std::string& text);
    void applyField(Field field, const std::string& text);
    /// True, having said why in the status line, when PANIC is engaged — IDENTIFY and TEST
    /// send nothing then (the audit's M17).
    bool refusedForPanic();
    void publishAll();
    void publishList();
    void publishSelected();
    /// The channel rows, written in place — see `writeRows`.
    void publishChannels();
    ChannelRow rowFor(const dmx::Fixture& fixture, std::size_t index,
                      const std::vector<std::uint8_t>& levels) const;
    /// Just the live levels, which is all `tick` needs to touch on a settled window.
    void publishLevels();
    void setStatus(const std::string& text, bool error);
    void resettle(int wanted);
    /// Which built-in mode this fixture's channel map matches, or 0 for "custom". Recomputed
    /// after every edit, so the box cannot go on naming a mode the map no longer is.
    int modeOf(const dmx::Fixture& fixture) const;

    output::OutputRunner& runner_;
    std::vector<dmx::Fixture> fixtures_;
    int selected_ = -1;
    PatchChanged changed_;

    slint::ComponentHandle<FixturesWindow> window_;
    std::shared_ptr<slint::VectorModel<FixtureRow>> listModel_;
    /// A fixture row's ×, and a channel row's: a double-click on either is one deletion. And a
    /// row's copy mark, where a double-click made two copies on the same channels.
    DeleteGuard fixtureMarks_;
    DeleteGuard channelMarks_;
    DeleteGuard copyMarks_;
    /// Redraws since the summary line was last worked out — see `tick`.
    int summaryTicks_ = 0;
    std::shared_ptr<slint::VectorModel<ChannelRow>> channelModel_;
    bool visible_ = false;
    /// A number typed into the address, the TEST level or a channel's "parked at", not yet
    /// entered — for the fixture it was typed for (`fixtureId`, empty for the TEST level, which
    /// is the window's).
    ///
    /// **A box's own commit counts only against this.** A box commits on Enter or on losing the
    /// keyboard, and the second runs a turn of the event loop after the click that took it — by
    /// when that click may have put another fixture in the editor. If that fixture holds what the
    /// first one did there — two pars at universe 0, two channels parked at 0 — nothing changes
    /// the box, which goes on holding what was typed, and its late commit lands it on the second
    /// fixture (the audit of 2026-09-25, H10, M18; measured 2026-10-01: with this check taken
    /// out, "what is typed in the patch editor goes to the fixture it was typed for" fails on the
    /// universe and the parked level). So every action commits what was typed first
    /// (`commitDrafts`), and a commit nothing was typed for — or typed for a fixture no longer
    /// showing — is dropped.
    struct Typed {
        std::string fixtureId;
        Numbered box = Numbered::Address;
        int index = 0;
        int value = 0;
    };
    std::optional<Typed> typed_;
    /// The same for the name, the group and the universe.
    struct Draft {
        std::string fixtureId;
        Field field = Field::Name;
        std::string text;
    };
    std::optional<Draft> draft_;
    /// What TEST sends. Full by default: an operator poking a channel to find out what it
    /// does wants the answer to be unmistakable.
    int testLevel_ = 255;
    std::string status_;
};

} // namespace takt4::ui
