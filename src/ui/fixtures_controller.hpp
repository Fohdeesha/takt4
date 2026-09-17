#pragma once

#include "core/dmx/fixture.hpp"
#include "core/output/output_runner.hpp"

#include "main_window.h" // generated; holds FixturesWindow too — see src/ui/CMakeLists.txt

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
/// Replacing the patch a few times a second is cheap and, more to the point, *safe*: levels
/// survive a re-patch where they can (`DmxEngine::setPatch`), so an operator typing an address
/// while the rig is lit does not make it blink on every keystroke.
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

    /// One round of refreshing what the window shows — the live levels, and the summary line.
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

    // What the window's callbacks do, reachable directly as well as through a click — which is
    // how `takt4_ui_tests` drives them, Slint's element-finding API being behind
    // SLINT_FEATURE_EXPERIMENTAL (§6).
    void pick(int index);
    void add();
    void remove();
    void removeAt(int index);
    void duplicate();
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
    /// Drives every channel of the selected fixture to full for `kIdentifySeconds`, so an
    /// operator in the truss can see which lamp they are patching.
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
    void publishAll();
    void publishList();
    void publishSelected();
    /// Updates the channel rows in place, or marks them for rebuilding when the shape has
    /// changed. See its definition for why a rebuild cannot happen here.
    void publishChannels();
    /// Builds the channel rows from nothing, so every box in them comes back *bound*. Called
    /// by `tick` when `publishChannels` found a shape it could not honestly update in place.
    void rebuildChannels();
    ChannelRow rowFor(const dmx::Fixture& fixture, std::size_t index) const;
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
    std::shared_ptr<slint::VectorModel<ChannelRow>> channelModel_;
    bool visible_ = false;
    /// Which fixture the channel rows currently on screen were built for, and how many. When
    /// either changes the rows are rebuilt rather than updated in place, so every box comes
    /// back *bound* — the same trap `RulesController::publishSlots` documents, where a box
    /// that has been typed into stops following the model.
    int channelsBuiltFor_ = -1;
    std::size_t channelsShown_ = 0;
    /// Set by `publishChannels` when it found a shape it could not update in place, and
    /// consumed by `tick`. **Deferred rather than done there and then**, because a publisher
    /// runs inside the callback of the very widget being replaced.
    bool channelsDirty_ = false;
    /// What TEST sends. Full by default: an operator poking a channel to find out what it
    /// does wants the answer to be unmistakable.
    int testLevel_ = 255;
    std::string status_;
};

} // namespace takt4::ui
