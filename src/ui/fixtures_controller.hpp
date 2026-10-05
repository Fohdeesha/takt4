#pragma once

#include "core/dmx/fixture.hpp"
#include "core/fixtures/fixture_profile.hpp"
#include "core/output/output_runner.hpp"
#include "core/settings/settings.hpp"
#include "ui/delete_guard.hpp"

#include "main_window.h" // generated; holds FixturesWindow too — see src/ui/CMakeLists.txt

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
    /// The same for the preset's library of imported definitions (`settings::Preset::library`),
    /// called only when it changed: an import adds to it, a re-import replaces an entry, a × takes
    /// an unused one out.
    using LibraryChanged = std::function<void(const std::vector<fixtures::FixtureProfile>&)>;

    /// The runner must outlive this. Nothing is shown until `show()`.
    FixturesController(output::OutputRunner& runner, std::vector<dmx::Fixture> fixtures,
                       std::vector<fixtures::FixtureProfile> library = {});

    FixturesController(const FixturesController&) = delete;
    FixturesController& operator=(const FixturesController&) = delete;

    void setPatchChanged(PatchChanged changed) { changed_ = std::move(changed); }
    void setLibraryChanged(LibraryChanged changed) { libraryChanged_ = std::move(changed); }

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
    const std::vector<fixtures::FixtureProfile>& library() const noexcept { return library_; }
    /// Replaces the patch from outside — a preset load. Keeps the selection where it can, and
    /// closes an import: what it was importing into is gone. The library stays as it is.
    void setFixtures(std::vector<dmx::Fixture> fixtures);
    /// The same with the library that came with them — an IMPORT of settings.
    void setFixtures(std::vector<dmx::Fixture> fixtures,
                     std::vector<fixtures::FixtureProfile> library);

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
    /// For a fixture patched by hand, index 0 is "custom" and changes nothing, and the rest index
    /// `dmx::builtinModes()`. For one an import made, the dropdown is its definition's modes — of
    /// its own shape, one start address or two (`fixtures::modesFor`) — with the mode it is in
    /// first as "… (edited)" while its channels differ from the definition's; picking a mode puts
    /// its channels, parked levels and names back as imported, and for one of a pair re-modes both
    /// (`fixtures::remode`).
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

    // --- the import (the operator's decisions of 2026-10-04 and 2026-10-05) ----------------------
    // In the editor's place while it is open; the editor is kept behind it, and what was being
    // typed there is committed before it opens, so nothing typed is lost to it.

    /// IMPORT beside the list's +: the import opens, with the universe of the fixture showing and
    /// the first free address after everything patched on it.
    void openImport();
    /// CANCEL: back to the editor, as it was. What the import already did to the library — a
    /// replace answered, an entry taken out — stays done.
    void closeImport();
    bool importing() const noexcept { return import_.open; }
    /// A's library, by its row as the sheet lists it (`fixtures::libraryOrder`).
    void pickLibraryEntry(int row);
    /// An unused entry's ×. One a fixture is made from cannot be taken out, and has no ×.
    void removeLibraryEntry(int row);
    /// FROM A FILE…: the open dialog, then `importFile` with what was picked.
    void chooseImportFile();
    /// A definition file read and mapped: new to the library, the same as an entry of it (which
    /// is picked), or another version of one (which asks — `answerReplace` and the rest). Or why
    /// it could not be read, in A. **Tests call this in place of the dialog**, which they cannot
    /// answer (`tests/support/crt_dialogs.cpp`).
    void importFile(const std::filesystem::path& path);
    /// The three answers to a file the library has another version of.
    void answerReplace();
    void answerKeepBoth();
    void answerCancel();
    /// B: a mode, by its row. A mode that cannot be imported cannot be picked.
    void pickImportMode(int row);
    /// C's boxes.
    void setImportCount(int count);
    void setImportUniverse(const std::string& text);
    void setImportAddress(int address);
    /// IMPORT: the fixtures made, added after the last, the first of them picked — and a
    /// definition from a file added to the library. Refused, with the reason in C, when the block
    /// runs past the universe.
    void confirmImport();

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
    /// Tells the owner the library changed, so it is saved with the preset.
    void commitLibrary();
    /// Applies what is being typed — in a text box or a number box — to the fixture it was typed
    /// for. Called before **every** action — see the constructor, where each callback is wired.
    void commitDrafts();

    /// The number boxes, whose keystrokes are kept until they are entered — see `typed_`. The
    /// TEST level and the import's two are the window's, not a fixture's.
    enum class Numbered : std::uint8_t { Address, TestLevel, Parked, ImportCount, ImportAddress };
    /// And the text boxes — see `draft_`. The import's universe is the window's.
    enum class Field : std::uint8_t { Name, Group, Universe, ImportUniverse };
    /// Whether a box belongs to the window rather than to the fixture showing.
    static bool windowsOwn(Numbered box) noexcept {
        return box == Numbered::TestLevel || box == Numbered::ImportCount ||
               box == Numbered::ImportAddress;
    }
    static bool windowsOwn(Field field) noexcept { return field == Field::ImportUniverse; }
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
    /// The library entry a fixture is linked to, or null — unlinked, or linked to nothing there.
    const fixtures::FixtureProfile* profileOf(const dmx::Fixture& fixture) const noexcept;
    /// The mode dropdown's entries for the fixture showing — its definition's or the built-in
    /// shapes — and which it is in. See `pickMode`.
    void publishModes(const dmx::Fixture* fixture);
    /// What a channel's note is: the definition's note for the channel of that name nearest it
    /// in the mode, so it stays with its channel when one above was taken out.
    std::string noteFor(const dmx::Fixture& fixture, std::size_t index) const;

    /// The definition the import is on: a library entry, or the file read, or none.
    const fixtures::FixtureProfile* importProfile() const noexcept;
    /// Everything the import's sheets show, written in place.
    void publishImport();
    /// Picks the first mode of the definition that can be imported.
    void pickFirstImportMode();
    /// The file's state cleared: nothing read, nothing asked.
    void forgetImportFile();

    output::OutputRunner& runner_;
    std::vector<dmx::Fixture> fixtures_;
    /// The preset's library — see `settings::Preset::library`.
    std::vector<fixtures::FixtureProfile> library_;
    int selected_ = -1;
    PatchChanged changed_;
    LibraryChanged libraryChanged_;

    /// The import, while it is open.
    struct Import {
        bool open = false;
        /// The library entry picked, by id; empty for none — or for a file's definition the
        /// library has not got (`incoming`).
        std::string entryId;
        /// A file's definition, read and mapped, not in the library: what IMPORT adds to it.
        std::optional<fixtures::FixtureProfile> incoming;
        /// A file's definition the library has another version of — `incoming` holds it — and
        /// the entry it differs from: the question asked before anything changes.
        std::optional<std::string> askingAbout;
        /// What A says about the file: its name and maker, and under it what it is to the
        /// library, or why it could not be read.
        std::string fileLine;
        std::string fileNote;
        bool fileError = false;
        std::string mode;
        int count = 1;
        dmx::PortAddress universe = 0;
        int address = 1;
    };
    Import import_;
    /// The library's rows as A lists them: `library_` indices, by maker and model.
    std::vector<std::size_t> libraryOrder_;
    std::shared_ptr<slint::VectorModel<LibraryRow>> libraryModel_;
    std::shared_ptr<slint::VectorModel<ModeRow>> importModesModel_;
    /// The mode dropdown's entries, its asides, and — for a fixture an import made — the mode
    /// name behind each entry ("" for the "… (edited)" one, which picks nothing).
    std::shared_ptr<slint::VectorModel<slint::SharedString>> modesModel_;
    std::shared_ptr<slint::VectorModel<slint::SharedString>> modeAsidesModel_;
    std::vector<std::string> modeNames_;
    /// A library entry's ×: a double-click is one removal.
    DeleteGuard libraryMarks_;

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
    /// The same for the name, the group and the universe — and the import's universe, which is the
    /// window's (`fixtureId` empty).
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
