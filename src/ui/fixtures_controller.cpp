#include "ui/fixtures_controller.hpp"

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/effect.hpp"
#include "core/fixtures/fixture_library.hpp"
#include "core/fixtures/import.hpp"
#include "core/io/utf8.hpp"
#include "ui/file_dialog.hpp"
#include "ui/model_rows.hpp"
#include "ui/native_window.hpp"
#include "ui/window_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>

namespace takt4::ui {
namespace {

/// Every string this editor shows, made safe to show — see `io::validUtf8`.
slint::SharedString shared(const std::string& text) {
    return slint::SharedString(io::validUtf8(text));
}

/// "0 · 1-3" — the universe and the channels the fixture occupies, spelled the way the
/// display on the back of the fixture spells them.
std::string whereOf(const dmx::Fixture& fixture) {
    std::string text = dmx::describePortAddress(fixture.universe) + " · ";
    text += std::to_string(static_cast<unsigned int>(fixture.address));
    if (fixture.channels.size() > 1) {
        text += "-" + std::to_string(static_cast<unsigned int>(dmx::lastChannelOf(fixture)));
    }
    return text;
}

/// A name nothing in `patch` is using yet — "fixture 1", "fixture 2".
///
/// Unique because two fixtures called the same thing are two rows nobody can tell apart in the
/// rule editor's list, which is never what an operator pressing ADD twice meant. Nothing
/// *routes* by it any more; see `dmx::Fixture::id`.
std::string freshName(const std::vector<dmx::Fixture>& patch, const std::string& stem) {
    for (int suffix = 1; suffix < 1000; ++suffix) {
        const std::string candidate = stem + " " + std::to_string(suffix);
        const bool taken = std::any_of(patch.begin(), patch.end(), [&](const dmx::Fixture& other) {
            return other.name == candidate;
        });
        if (!taken) {
            return candidate;
        }
    }
    return stem;
}

} // namespace

FixturesController::FixturesController(output::OutputRunner& runner,
                                       std::vector<dmx::Fixture> fixtures,
                                       std::vector<fixtures::FixtureProfile> library)
    : runner_(runner), fixtures_(std::move(fixtures)), library_(std::move(library)),
      libraryModel_(std::make_shared<slint::VectorModel<LibraryRow>>()),
      importModesModel_(std::make_shared<slint::VectorModel<ModeRow>>()),
      modesModel_(std::make_shared<slint::VectorModel<slint::SharedString>>()),
      modeAsidesModel_(std::make_shared<slint::VectorModel<slint::SharedString>>()),
      window_(FixturesWindow::create()),
      listModel_(std::make_shared<slint::VectorModel<FixtureRow>>()),
      channelModel_(std::make_shared<slint::VectorModel<ChannelRow>>()) {
    // A patch built in code, or read by something older than the settings loader, may have
    // none; the settings loader has already given every fixture one.
    dmx::ensureFixtureIds(fixtures_);
    window_->set_fixtures(listModel_);
    window_->set_channels(channelModel_);
    window_->set_library_rows(libraryModel_);
    window_->set_mode_rows(importModesModel_);
    // The mode dropdown's entries are the fixture's — its definition's modes, or the built-in
    // shapes — so they are written for each fixture shown (`publishModes`), in place.
    window_->set_modes(modesModel_);
    window_->set_mode_asides(modeAsidesModel_);

    auto roles = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Role role : dmx::kRoles) {
        roles->push_back(shared(std::string(dmx::labelOf(role))));
    }
    window_->set_roles(roles);

    // **Every action commits what the boxes hold before it runs** (the audit of 2026-09-25, M18).
    // `commitDrafts` used to run only when the selection moved, so a name, a group or a universe
    // typed and not entered reverted on ADD CHANNEL, a channel's ×, or another fixture's dot. A
    // box's own commit is not wrapped; it is the one box with the keyboard, and it counts only if
    // it was typed in (`typed_`, `draft_`).
    const auto finishing = [this](auto action) {
        return [this, action](auto... args) {
            commitDrafts();
            action(args...);
        };
    };

    window_->on_picked([this](int index) { pick(index); }); // `pick` commits them itself
    window_->on_added(finishing([this] { add(); }));
    // Guarded as × is: a double-click on copy made two copies, on the same channels.
    window_->on_duplicated_at(finishing([this](int index) {
        if (copyMarks_.press(index)) {
            duplicateAt(index);
        }
    }));
    // Through `DeleteGuard`, which drops the second click of a double-click on ×: the row
    // below moves up under the pointer and would take it (the audit of 2026-09-25, L10).
    window_->on_removed_at(finishing([this](int index) {
        if (fixtureMarks_.press(index)) {
            removeAt(index);
        }
    }));
    window_->on_enabled_changed(
        finishing([this](int index, bool on) { setEnabledAt(index, on); }));
    window_->on_name_typed(
        [this](const slint::SharedString& text) { noteDraft(Field::Name, std::string(text)); });
    window_->on_name_edited(
        [this](const slint::SharedString& text) { finishDraft(Field::Name, std::string(text)); });
    window_->on_group_typed(
        [this](const slint::SharedString& text) { noteDraft(Field::Group, std::string(text)); });
    window_->on_group_edited(
        [this](const slint::SharedString& text) { finishDraft(Field::Group, std::string(text)); });
    window_->on_universe_typed([this](const slint::SharedString& text) {
        noteDraft(Field::Universe, std::string(text));
    });
    window_->on_universe_edited([this](const slint::SharedString& text) {
        finishDraft(Field::Universe, std::string(text));
    });
    window_->on_address_changed([this](int address) {
        if (takeTyped(Numbered::Address, 0)) {
            setAddress(address);
        }
    });
    window_->on_address_typed(
        [this](int address) { noteTyped(Numbered::Address, 0, address); });
    window_->on_fixture_enabled_changed(finishing([this](bool on) { setEnabled(on); }));
    window_->on_mode_picked(finishing([this](int mode) { pickMode(mode); }));
    window_->on_channel_added(finishing([this] { addChannel(); }));
    window_->on_channel_removed(finishing([this](int index) {
        if (channelMarks_.press(index)) { // a double-click on × is one deletion (L10)
            removeChannel(index);
        }
    }));
    window_->on_channel_role_picked(
        finishing([this](int index, int role) { pickChannelRole(index, role); }));
    window_->on_channel_parked_changed([this](int index, int level) {
        if (takeTyped(Numbered::Parked, index)) {
            setChannelParked(index, level);
        }
    });
    window_->on_channel_parked_typed(
        [this](int index, int level) { noteTyped(Numbered::Parked, index, level); });
    window_->on_pan_range_changed(
        finishing([this](float low, float high) { setPanRange(low, high); }));
    window_->on_tilt_range_changed(
        finishing([this](float low, float high) { setTiltRange(low, high); }));
    window_->on_identify(finishing([this] { identify(); }));
    window_->on_channel_tested(finishing([this](int index) { testChannel(index); }));
    window_->on_test_level_changed([this](int level) {
        if (takeTyped(Numbered::TestLevel, 0)) {
            setTestLevel(level);
        }
    });
    window_->on_test_level_typed(
        [this](int level) { noteTyped(Numbered::TestLevel, 0, level); });
    window_->on_fold_clicked(finishing([this](int section) { fold(section); }));

    // The import. Every button through `finishing`, as the editor's are: IMPORT itself commits
    // what was being typed in the editor before the import takes its place (the operator,
    // 2026-10-05: "be sure to not lose edits that were made in the editor it replaces"), and
    // the import's own IMPORT commits what was typed into its boxes before it reads them.
    window_->on_import_clicked(finishing([this] { openImport(); }));
    window_->on_import_cancelled(finishing([this] { closeImport(); }));
    window_->on_library_picked(finishing([this](int row) { pickLibraryEntry(row); }));
    window_->on_library_removed(finishing([this](int row) {
        if (libraryMarks_.press(row)) { // a double-click on × is one removal (L10)
            removeLibraryEntry(row);
        }
    }));
    window_->on_import_file_clicked(finishing([this] { chooseImportFile(); }));
    window_->on_ask_replace_clicked(finishing([this] { answerReplace(); }));
    window_->on_ask_keep_clicked(finishing([this] { answerKeepBoth(); }));
    window_->on_ask_cancel_clicked(finishing([this] { answerCancel(); }));
    window_->on_import_mode_picked(finishing([this](int row) { pickImportMode(row); }));
    window_->on_import_count_typed(
        [this](int count) { noteTyped(Numbered::ImportCount, 0, count); });
    window_->on_import_count_changed([this](int count) {
        if (takeTyped(Numbered::ImportCount, 0)) {
            setImportCount(count);
        }
    });
    window_->on_import_address_typed(
        [this](int address) { noteTyped(Numbered::ImportAddress, 0, address); });
    window_->on_import_address_changed([this](int address) {
        if (takeTyped(Numbered::ImportAddress, 0)) {
            setImportAddress(address);
        }
    });
    window_->on_import_universe_typed([this](const slint::SharedString& text) {
        noteDraft(Field::ImportUniverse, std::string(text));
    });
    window_->on_import_universe_edited([this](const slint::SharedString& text) {
        finishDraft(Field::ImportUniverse, std::string(text));
    });
    window_->on_import_confirmed(finishing([this] { confirmImport(); }));

    window_->set_test_level(testLevel_);
    window_->set_test_seconds(static_cast<int>(kTestSeconds));

    // What the window opens at. **Set from C++ and not from the markup**, for the reason
    // `ui::kRulesWindowWidth` gives at length: Slint sizes a window from what its content
    // asks for, so a `preferred-width` in the markup decides nothing and this window opened
    // at its 820x520 minimum — a patch editor two thirds the size of the rule editor beside
    // it. Asked for on a rig on 2026-09-16, measured from the shot they sent.
    // No larger than the screen has room for, though: see `fitToScreen` (the audit's M26).
    const LogicalExtent opening = fitToScreen({kFixturesWindowWidth, kFixturesWindowHeight});
    window_->window().set_size(slint::LogicalSize({opening.width, opening.height}));

    // The window's own close box goes through `hide` like CLOSE does. It used to hide the window
    // behind this object's back: `visible_` stayed set, and the levels went on being redrawn thirty
    // times a second for a window nobody could see, and a name being typed was not committed (the
    // audit of 2026-09-25, M16).
    window_->window().on_close_requested([this] {
        hide();
        return slint::CloseRequestResponse::HideWindow;
    });

    resettle(fixtures_.empty() ? -1 : 0);
    publishAll();
}

void FixturesController::show() {
    visible_ = true;
    window_->show();
    publishAll();
    // And brought forward, as the rule editor is (`RulesController::show`): `show()` leaves a
    // window that is already up where it is in the Z order, so the button looked dead with the
    // patch behind the main window. "fixtures" is in this window's title and no other of ours.
    (void)bringWindowToFront("fixtures");
}

void FixturesController::hide() {
    commitDrafts();
    visible_ = false;
    window_->hide();
}

dmx::Fixture* FixturesController::current() noexcept {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= fixtures_.size()) {
        return nullptr;
    }
    return &fixtures_[static_cast<std::size_t>(selected_)];
}

const dmx::Fixture* FixturesController::current() const noexcept {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= fixtures_.size()) {
        return nullptr;
    }
    return &fixtures_[static_cast<std::size_t>(selected_)];
}

void FixturesController::resettle(int wanted) {
    if (fixtures_.empty()) {
        selected_ = -1;
        return;
    }
    selected_ = std::clamp(wanted, 0, static_cast<int>(fixtures_.size()) - 1);
}

void FixturesController::commit() {
    runner_.post(output::OutputCommand::patch(fixtures_));
    if (changed_) {
        changed_(fixtures_);
    }
    publishAll();
}

void FixturesController::commitLibrary() {
    if (libraryChanged_) {
        libraryChanged_(library_);
    }
}

void FixturesController::setFixtures(std::vector<dmx::Fixture> fixtures) {
    fixtures_ = std::move(fixtures);
    dmx::ensureFixtureIds(fixtures_);
    resettle(selected_);
    // A new patch, so nothing half-typed is carried into it — not even into a fixture that happens
    // to carry the id of the one showing: ids are only unique within a patch, and an imported show
    // is another patch (the audit of 2026-09-25, M21).
    typed_.reset();
    draft_.reset();
    // Nor an import, whose readouts were about the patch that was here.
    import_ = Import{};
    window_->set_importing(false);
    publishAll();
}

void FixturesController::setFixtures(std::vector<dmx::Fixture> fixtures,
                                     std::vector<fixtures::FixtureProfile> library) {
    library_ = std::move(library);
    setFixtures(std::move(fixtures));
}

int FixturesController::modeOf(const dmx::Fixture& fixture) const {
    const auto modes = dmx::builtinModes();
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const std::span<const dmx::Role> channels = modes[i].channels;
        if (fixture.channels.size() == channels.size() &&
            std::equal(channels.begin(), channels.end(), fixture.channels.begin())) {
            return static_cast<int>(i) + 1; // 0 is "custom"
        }
    }
    return 0;
}

void FixturesController::publishAll() {
    publishList();
    publishSelected();
    publishChannels();
    publishImport();
    window_->set_summary(shared(summary()));
}

namespace {

FixtureRow rowOf(const dmx::Fixture& fixture) {
    FixtureRow row{};
    row.name = shared(fixture.name);
    row.where = shared(whereOf(fixture));
    row.group = shared(fixture.group);
    row.enabled = fixture.enabled;
    row.problem = shared(dmx::problemWith(fixture));
    return row;
}

} // namespace

void FixturesController::publishList() {
    std::vector<FixtureRow> rows;
    rows.reserve(fixtures_.size());
    for (const dmx::Fixture& fixture : fixtures_) {
        rows.push_back(rowOf(fixture));
    }
    // In place: a row's dot, a row's marks and the box being typed in survive an edit elsewhere.
    writeRows(*listModel_, rows);
    window_->set_selected(selected_);
}

void FixturesController::publishSelected() {
    const dmx::Fixture* const fixture = current();
    publishModes(fixture);
    if (fixture == nullptr) {
        window_->set_name(shared({}));
        window_->set_group(shared({}));
        window_->set_universe(shared({}));
        window_->set_address(1);
        window_->set_enabled(true);
        window_->set_moves(false);
        return;
    }
    window_->set_name(shared(fixture->name));
    window_->set_group(shared(fixture->group));
    window_->set_universe(shared(dmx::describePortAddress(fixture->universe)));
    window_->set_address(static_cast<int>(fixture->address));
    window_->set_enabled(fixture->enabled);
    // The movement window is shown for a fixture that can move, and hidden for one that
    // cannot — on a wash the four numbers mean nothing and would be four controls an operator
    // has to work out are irrelevant.
    window_->set_moves(dmx::has(*fixture, dmx::Role::Pan) || dmx::has(*fixture, dmx::Role::Tilt));
    window_->set_pan_min(static_cast<float>(fixture->panMin * 100.0));
    window_->set_pan_max(static_cast<float>(fixture->panMax * 100.0));
    window_->set_tilt_min(static_cast<float>(fixture->tiltMin * 100.0));
    window_->set_tilt_max(static_cast<float>(fixture->tiltMax * 100.0));
}

const fixtures::FixtureProfile*
FixturesController::profileOf(const dmx::Fixture& fixture) const noexcept {
    return fixture.profile.linked() ? fixtures::findProfile(library_, fixture.profile.id) : nullptr;
}

void FixturesController::publishModes(const dmx::Fixture* fixture) {
    std::vector<slint::SharedString> entries;
    std::vector<slint::SharedString> asides;
    modeNames_.clear();
    int index = 0;
    const fixtures::FixtureProfile* const profile =
        fixture == nullptr ? nullptr : profileOf(*fixture);
    const std::vector<std::string> names =
        profile == nullptr ? std::vector<std::string>{} : fixtures::modesFor(*fixture, *profile);
    if (profile != nullptr && !names.empty()) {
        // **The definition's modes**, of this fixture's shape. One whose channels were changed by
        // hand is in its mode no longer and says so, first — so that picking the mode itself,
        // below it, puts the channels back (a dropdown does not pick what it already shows).
        const bool edited =
            fixtures::isEdited(*fixture, *profile) ||
            std::find(names.begin(), names.end(), fixture->profile.mode) == names.end();
        if (edited) {
            entries.push_back(shared(fixture->profile.mode + " (edited)"));
            asides.push_back(shared(std::to_string(fixture->channels.size())));
            modeNames_.emplace_back();
        }
        for (const std::string& name : names) {
            if (!edited && name == fixture->profile.mode) {
                index = static_cast<int>(entries.size());
            }
            entries.push_back(shared(name));
            const fixtures::ProfileMode* const mode = fixtures::findMode(*profile, name);
            asides.push_back(
                shared(mode == nullptr ? std::string() : fixtures::channelCountOf(*mode)));
            modeNames_.push_back(name);
        }
        window_->set_mode_footnote(shared("picking one puts its channels back as imported"));
        window_->set_mode_note(
            shared("from " +
                   (profile->manufacturer.empty() ? std::string() : profile->manufacturer + " · ") +
                   fixtures::displayName(library_, *profile)));
    } else {
        // Index 0 is "custom", so that a channel map the operator has edited has something
        // honest to be — and so picking nothing is a state rather than a fixture nobody meant.
        // Worded as what it is, since picking it changes nothing (see `pickMode`).
        entries.push_back(shared("custom (the channels below)"));
        for (const dmx::FixtureMode& mode : dmx::builtinModes()) {
            entries.push_back(shared(std::string(mode.name)));
        }
        index = fixture == nullptr ? 0 : modeOf(*fixture);
        window_->set_mode_footnote(shared({}));
        window_->set_mode_note(shared("fills in the channels below"));
    }
    writeRows(*modesModel_, entries);
    writeRows(*modeAsidesModel_, asides);
    window_->set_mode_index(index);
}

std::string FixturesController::noteFor(const dmx::Fixture& fixture, std::size_t index) const {
    const fixtures::FixtureProfile* const profile = profileOf(fixture);
    const std::string_view label = dmx::channelLabel(fixture, index);
    if (profile == nullptr || label.empty()) {
        return {};
    }
    const fixtures::ProfileMode* const mode = fixtures::findMode(*profile, fixture.profile.mode);
    const int part = fixture.profile.part;
    if (mode == nullptr || part < 1 || static_cast<std::size_t>(part) > mode->parts.size()) {
        return {};
    }
    // The definition's channel of the same name nearest this one — at the same place unless a
    // channel above it was added or taken out by hand — so a note stays with its channel.
    const std::vector<fixtures::ProfileChannel>& channels =
        mode->parts[static_cast<std::size_t>(part - 1)];
    const fixtures::ProfileChannel* nearest = nullptr;
    std::size_t distance = 0;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].label != label) {
            continue;
        }
        const std::size_t away = i > index ? i - index : index - i;
        if (nearest == nullptr || away < distance) {
            nearest = &channels[i];
            distance = away;
        }
    }
    return nearest == nullptr ? std::string() : nearest->note;
}

ChannelRow FixturesController::rowFor(const dmx::Fixture& fixture, std::size_t index,
                                     const std::vector<std::uint8_t>& levels) const {
    ChannelRow row{};
    row.number = static_cast<int>(fixture.address) + static_cast<int>(index);
    row.label = shared(std::string(dmx::channelLabel(fixture, index)));
    row.note = shared(noteFor(fixture, index));
    row.role_index = 0;
    for (std::size_t r = 0; r < dmx::kRoles.size(); ++r) {
        if (dmx::kRoles[r] == fixture.channels[index]) {
            row.role_index = static_cast<int>(r);
            break;
        }
    }
    row.parked = index < fixture.parked.size() ? static_cast<int>(fixture.parked[index]) : 0;
    // Which head it moves, as the engine counts them (`dmx::headsOf`): the nth pan, tilt or fine
    // is head n's — a pan the heads share is the first's. Only where there is more than one.
    const dmx::Role role = fixture.channels[index];
    if (dmx::headsOf(fixture) >= 2 && (role == dmx::Role::Pan || role == dmx::Role::Tilt ||
                                       role == dmx::Role::PanFine || role == dmx::Role::TiltFine)) {
        const auto before =
            std::count(fixture.channels.begin(),
                       fixture.channels.begin() + static_cast<std::ptrdiff_t>(index), role);
        row.head = static_cast<int>(before) + 1;
    }
    const std::size_t channel = std::size_t{fixture.address} + index;
    row.live = channel >= 1 && channel <= levels.size() ? static_cast<int>(levels[channel - 1]) : 0;
    return row;
}

void FixturesController::publishChannels() {
    const dmx::Fixture* const fixture = current();
    std::vector<ChannelRow> rows;
    if (fixture != nullptr) {
        // Through the runner's mirror — see `publishLevels`.
        const std::vector<std::uint8_t> levels = runner_.levelsOf(fixture->universe);
        rows.reserve(fixture->channels.size());
        for (std::size_t i = 0; i < fixture->channels.size(); ++i) {
            rows.push_back(rowFor(*fixture, i, levels));
        }
    }
    // A column for what each channel is called, only for a fixture that has names for them.
    window_->set_has_labels(fixture != nullptr &&
                            std::any_of(fixture->labels.begin(), fixture->labels.end(),
                                        [](const std::string& label) { return !label.empty(); }));
    // And one for which head each pan and tilt moves, only for a fixture with more than one.
    window_->set_has_heads(fixture != nullptr && dmx::headsOf(*fixture) >= 2);
    // **In place, always** — another fixture, a new mode, a channel added or taken away. Each row
    // shows what its model says and nothing of its own (weltformat.slint), so updating it is
    // enough, and rebuilding was what took a dropdown away under the pointer and left a box
    // showing the last fixture's pick (the audit of 2026-09-25, M21, T3). The rows a fixture with
    // fewer channels has not got are erased from the end; a box being typed in commits first
    // (`commitDrafts`), whatever row it is on.
    writeRows(*channelModel_, rows);
}

void FixturesController::publishLevels() {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr || channelModel_->row_count() == 0) {
        return;
    }
    // Through the runner's mirror, never through the live engine: the buffers belong to the
    // output thread and the vector holding them is *replaced* on every re-patch, so a 30 Hz
    // readout walking it would be reading a freed buffer the moment somebody typed an address.
    // See `OutputRunner::levelsOf`.
    const std::vector<std::uint8_t> levels = runner_.levelsOf(fixture->universe);
    for (std::size_t i = 0; i < channelModel_->row_count(); ++i) {
        ChannelRow row = *channelModel_->row_data(i);
        const std::size_t channel = std::size_t{fixture->address} + i;
        const int live =
            channel >= 1 && channel <= levels.size() ? static_cast<int>(levels[channel - 1]) : 0;
        if (row.live != live) {
            row.live = live;
            channelModel_->set_row_data(i, row);
        }
    }
}

void FixturesController::tick() {
    if (!visible_) {
        return;
    }
    publishLevels();
    // And the line under the list about once a second: it says whether an Art-Net output is
    // carrying the patch, which the main window can change with this window open.
    if (++summaryTicks_ >= 30) {
        summaryTicks_ = 0;
        window_->set_summary(shared(summary()));
    }
}

std::string FixturesController::summary() const {
    if (fixtures_.empty()) {
        return "Nothing patched yet.";
    }
    const std::vector<dmx::PortAddress> universes = dmx::universesOf(fixtures_);
    std::string text = std::to_string(fixtures_.size()) +
                       (fixtures_.size() == 1 ? " fixture" : " fixtures") + " on " +
                       std::to_string(universes.size()) +
                       (universes.size() == 1 ? " universe" : " universes");
    // And whether anything is actually carrying them, which is the half an operator cannot
    // see from a patch: a rig with a perfect patch and no Art-Net target sends nothing, and
    // looks exactly like a rig whose node is unplugged.
    const output::OutputRunner::Snapshot snapshot = runner_.snapshot();
    std::size_t nodes = 0;
    for (const output::OutputTarget& target : snapshot.outputs) {
        if (target.kind == output::OutputTarget::Kind::ArtNet && target.enabled) {
            ++nodes;
        }
    }
    if (nodes == 0) {
        text += " — no Art-Net output yet: add one in the main window";
    } else {
        text += ", going to " + std::to_string(nodes) + (nodes == 1 ? " node" : " nodes");
    }
    // And two fixtures on the same channels, which drive each other — a par that dims when the
    // head beside it pans — and which nothing said (the audit's M21). The first pair, and how
    // many more, since one is enough to go and look.
    const std::vector<dmx::Overlap> overlaps = dmx::overlappingFixtures(fixtures_);
    if (!overlaps.empty()) {
        const dmx::Overlap& one = overlaps.front();
        text += ". " + fixtures_[one.first].name + " and " + fixtures_[one.second].name +
                " share channel " + std::to_string(one.channel) + " of universe " +
                dmx::describePortAddress(fixtures_[one.first].universe);
        if (overlaps.size() > 1) {
            text += " (and " + std::to_string(overlaps.size() - 1) + " more overlap" +
                    (overlaps.size() == 2 ? "" : "s") + ")";
        }
    }
    return text;
}

void FixturesController::setStatus(const std::string& text, bool error) {
    status_ = text;
    window_->set_status(shared(text));
    window_->set_status_error(error);
}

void FixturesController::commitDrafts() {
    // A number still being typed, then a text: each for the fixture it was typed on, and only
    // one of them can be live — only one box has the keyboard. Taken before applied, so the
    // box's own late commit finds nothing left to count (see `typed_`).
    if (typed_) {
        const Typed pending = *typed_;
        typed_.reset();
        const dmx::Fixture* const fixture = current();
        if (pending.box == Numbered::TestLevel) {
            setTestLevel(pending.value);
        } else if (pending.box == Numbered::ImportCount) {
            setImportCount(pending.value);
        } else if (pending.box == Numbered::ImportAddress) {
            setImportAddress(pending.value);
        } else if (fixture != nullptr && fixture->id == pending.fixtureId) {
            if (pending.box == Numbered::Address) {
                setAddress(pending.value);
            } else {
                setChannelParked(pending.index, pending.value);
            }
        }
    }
    if (draft_) {
        const Draft pending = *draft_;
        draft_.reset();
        const dmx::Fixture* const fixture = current();
        if (windowsOwn(pending.field) || (fixture != nullptr && fixture->id == pending.fixtureId)) {
            applyField(pending.field, pending.text);
        }
    }
}

void FixturesController::noteTyped(Numbered box, int index, int value) {
    const dmx::Fixture* const fixture = current();
    if (!windowsOwn(box) && fixture == nullptr) {
        return;
    }
    const std::string id = windowsOwn(box) ? std::string() : fixture->id;
    // Another box's draft still standing means its own commit has not come yet — the keyboard
    // moved straight from it to this one. Committed now, before this one replaces it.
    const bool another = (typed_ && (typed_->box != box || typed_->index != index ||
                                     typed_->fixtureId != id)) ||
                         draft_.has_value();
    if (another) {
        commitDrafts();
    }
    typed_ = Typed{id, box, index, value};
}

bool FixturesController::takeTyped(Numbered box, int index) {
    const dmx::Fixture* const fixture = current();
    const std::string id = windowsOwn(box) || fixture == nullptr ? std::string() : fixture->id;
    if (!typed_ || typed_->box != box || typed_->index != index || typed_->fixtureId != id ||
        (!windowsOwn(box) && fixture == nullptr)) {
        return false;
    }
    typed_.reset();
    return true;
}

void FixturesController::noteDraft(Field field, const std::string& text) {
    const dmx::Fixture* const fixture = current();
    if (!windowsOwn(field) && fixture == nullptr) {
        return;
    }
    const std::string id = windowsOwn(field) ? std::string() : fixture->id;
    if ((draft_ && (draft_->field != field || draft_->fixtureId != id)) || typed_) {
        commitDrafts();
    }
    draft_ = Draft{id, field, text};
}

void FixturesController::finishDraft(Field field, const std::string& text) {
    const dmx::Fixture* const fixture = current();
    // Only an edit that was typed, for the fixture showing — see `typed_`. With nothing typed the
    // box is only letting go of what it was shown, which may be another fixture's by now. The
    // import's universe is the window's, typed for no fixture.
    const std::string id = windowsOwn(field)    ? std::string()
                           : fixture == nullptr ? std::string()
                                                : fixture->id;
    if ((!windowsOwn(field) && fixture == nullptr) || !draft_ || draft_->field != field ||
        draft_->fixtureId != id) {
        return;
    }
    draft_.reset();
    applyField(field, text);
}

void FixturesController::applyField(Field field, const std::string& text) {
    switch (field) {
    case Field::Name:
        rename(text);
        break;
    case Field::Group:
        setGroup(text);
        break;
    case Field::Universe:
        setUniverse(text);
        break;
    case Field::ImportUniverse:
        setImportUniverse(text);
        break;
    }
}

void FixturesController::pick(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixtures_.size()) {
        return;
    }
    if (index != selected_) {
        // What the line said was about the fixture before — "Identifying head 1…" over head 2.
        // Before the drafts are committed, so a universe refused on the way out is still said.
        setStatus({}, false);
    }
    commitDrafts();
    // A fixture clicked in the list while importing is the editor wanted back, on that fixture.
    if (import_.open) {
        closeImport();
    }
    selected_ = index;
    publishList();
    publishSelected();
    publishChannels();
}

void FixturesController::applyLayout(const settings::MachineSettings& machine) {
    window_->set_where_folded(machine.patchSectionsFolded[0]);
    window_->set_channels_folded(machine.patchSectionsFolded[1]);
    window_->set_moves_folded(machine.patchSectionsFolded[2]);
}

void FixturesController::layoutInto(settings::MachineSettings& machine) const {
    machine.patchSectionsFolded = {window_->get_where_folded(), window_->get_channels_folded(),
                                   window_->get_moves_folded()};
}

void FixturesController::fold(int section) {
    commitDrafts();
    switch (section) {
    case 0:
        window_->set_where_folded(!window_->get_where_folded());
        break;
    case 1:
        window_->set_channels_folded(!window_->get_channels_folded());
        break;
    case 2:
        window_->set_moves_folded(!window_->get_moves_folded());
        break;
    case 3:
        window_->set_import_type_folded(!window_->get_import_type_folded());
        break;
    case 4:
        window_->set_import_mode_folded(!window_->get_import_mode_folded());
        break;
    case 5:
        window_->set_import_where_folded(!window_->get_import_where_folded());
        break;
    default:
        break;
    }
}

void FixturesController::add() {
    commitDrafts();
    // What the line said was about the fixture showing, which this replaces.
    setStatus({}, false);
    // A fixture added by hand is the editor wanted, on it.
    if (import_.open) {
        closeImport();
    }
    if (fixtures_.size() >= dmx::kMaxRoutableFixtures) {
        // Past this a fixture can still be patched, parked and tested from here; what it
        // cannot be is reached by a rule, by name or by group, because a rule carries its
        // fixtures as a bit each (`dmx::resolveFixtures` stops at the last it can name). Said
        // out loud rather than discovered — see `dmx::kMaxRoutableFixtures`. Short enough for the
        // one line under the top row.
        setStatus("Rules reach the first " + std::to_string(dmx::kMaxRoutableFixtures) +
                      " fixtures only; no rule will reach this one.",
                  true);
    }
    // Patched after the last one on its universe rather than at 1, because that is what an
    // operator adding a second par means and it is the arithmetic they would otherwise do by
    // hand. A universe with nothing on it starts at 1.
    dmx::Fixture fixture = dmx::fixtureFromMode(freshName(fixtures_, "fixture"), 1, 0, 1);
    fixture.id = dmx::newFixtureId(fixtures_);
    if (!fixtures_.empty()) {
        const dmx::Fixture& last = fixtures_.back();
        fixture.universe = last.universe;
        const std::uint16_t after = dmx::lastChannelOf(last);
        fixture.address = static_cast<std::uint16_t>(
            std::min<int>(after + 1, static_cast<int>(dmx::kChannelsPerUniverse)));
    }
    fixtures_.push_back(std::move(fixture));
    resettle(static_cast<int>(fixtures_.size()) - 1);
    commit();
}

void FixturesController::duplicate() {
    commitDrafts();
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    dmx::Fixture copy = *fixture;
    copy.name = freshName(fixtures_, fixture->name);
    // A fixture of its own, which rules aimed at the original do not reach.
    copy.id = dmx::newFixtureId(fixtures_);
    // A copy of one of a two-address pair is not in that pair — changing the pair's mode must
    // not change it — and alone it is not the mode's shape any more, so it is not linked either.
    // A copy of a fixture of one address stays linked to the same definition and mode.
    if (!copy.profile.pair.empty()) {
        copy.profile = dmx::Fixture::ProfileLink{};
    }
    // Addressed after the one it came from, which is what duplicating a par in a row means.
    const std::uint16_t after = dmx::lastChannelOf(*fixture);
    copy.address = static_cast<std::uint16_t>(
        std::min<int>(after + 1, static_cast<int>(dmx::kChannelsPerUniverse)));
    setStatus({}, false);
    // Into the list's model at its own place, so the rows below keep their own elements.
    listModel_->insert(static_cast<std::size_t>(selected_) + 1, rowOf(copy));
    fixtures_.insert(fixtures_.begin() + selected_ + 1, std::move(copy));
    resettle(selected_ + 1);
    commit();
}

void FixturesController::duplicateAt(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixtures_.size()) {
        return;
    }
    commitDrafts();
    // The copy is picked, in the editor.
    if (import_.open) {
        closeImport();
    }
    selected_ = index;
    duplicate();
}

void FixturesController::remove() {
    commitDrafts();
    if (current() == nullptr) {
        return;
    }
    setStatus({}, false);
    // Out of the list's model at its own place: no element is handed its neighbour's data.
    listModel_->erase(static_cast<std::size_t>(selected_));
    fixtures_.erase(fixtures_.begin() + selected_);
    resettle(selected_);
    commit();
}

void FixturesController::removeAt(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixtures_.size()) {
        return;
    }
    commitDrafts();
    if (index == selected_) {
        setStatus({}, false); // it was about the fixture going
    }
    listModel_->erase(static_cast<std::size_t>(index));
    fixtures_.erase(fixtures_.begin() + index);
    resettle(selected_ > index ? selected_ - 1 : selected_);
    commit();
}

void FixturesController::setEnabledAt(int index, bool on) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixtures_.size()) {
        return;
    }
    fixtures_[static_cast<std::size_t>(index)].enabled = on;
    commit();
}

void FixturesController::rename(const std::string& name) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr || fixture->name == name) {
        return;
    }
    // Only the label. A rule aims at the fixture's id, so every rule aimed at it goes on
    // reaching it under its new name — it used to un-route them all (the audit's M28).
    fixture->name = name;
    commit();
}

void FixturesController::setGroup(const std::string& group) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr || fixture->group == group) {
        return;
    }
    fixture->group = group;
    commit();
}

void FixturesController::setUniverse(const std::string& text) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    dmx::PortAddress universe = 0;
    if (!dmx::parsePortAddress(text, universe)) {
        // Refused rather than clamped, because what a half-typed box holds is not a universe
        // and writing 0 into the patch on the way to "12" would move the fixture twice.
        setStatus("A universe is a number from 0 to 32767, or net:sub:uni past the first 256.",
                  true);
        return;
    }
    setStatus({}, false);
    if (fixture->universe == universe) {
        return;
    }
    fixture->universe = universe;
    commit();
}

void FixturesController::setAddress(int address) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    const auto clamped = static_cast<std::uint16_t>(
        std::clamp(address, 1, static_cast<int>(dmx::kChannelsPerUniverse)));
    if (fixture->address == clamped) {
        return;
    }
    fixture->address = clamped;
    commit();
}

void FixturesController::setEnabled(bool on) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr || fixture->enabled == on) {
        return;
    }
    fixture->enabled = on;
    commit();
}

void FixturesController::pickMode(int mode) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    // **A fixture an import made: its definition's modes** (`publishModes`). Its channels, parked
    // levels and names as the definition has them — the way back from an edit by hand — and both
    // fixtures of a two-address pair, each its own part.
    if (!modeNames_.empty()) {
        const fixtures::FixtureProfile* const profile = profileOf(*fixture);
        if (profile == nullptr || mode < 0 || static_cast<std::size_t>(mode) >= modeNames_.size() ||
            modeNames_[static_cast<std::size_t>(mode)].empty()) {
            publishModes(fixture); // the "… (edited)" entry, which is where it is: nothing
            return;
        }
        const std::string name = modeNames_[static_cast<std::size_t>(mode)];
        const bool pair = !fixture->profile.pair.empty();
        const std::string fixtureName = fixture->name;
        if (!fixtures::remode(fixtures_, static_cast<std::size_t>(selected_), *profile, name)) {
            publishModes(fixture);
            return;
        }
        commit();
        setStatus(pair ? "Both parts of " + fixtureName + "'s pair are in " + name + " now."
                       : std::string(),
                  false);
        return;
    }
    if (mode <= 0) {
        // **"custom" describes a map; it sets none.** Picked, it did nothing, and the dropdown
        // went on saying "custom" over a fixture that was still an RGB par (the audit of
        // 2026-09-25, L39). It goes back to naming the map, and the line says how a custom one
        // is made.
        window_->set_mode_index(modeOf(*fixture));
        if (modeOf(*fixture) != 0) {
            setStatus("Edit the channels below for a custom map; the mode then says custom.",
                      false);
        }
        return;
    }
    const auto modes = dmx::builtinModes();
    const auto index = static_cast<std::size_t>(mode - 1);
    if (index >= modes.size()) {
        return;
    }
    // The channel map and the parked levels, and nothing else: the name, group, universe,
    // address and movement window are the operator's. Picking a mode says "this fixture is
    // shaped like that", not "start again".
    fixture->channels.assign(modes[index].channels.begin(), modes[index].channels.end());
    fixture->parked.assign(modes[index].parked.begin(), modes[index].parked.end());
    // A built-in shape is not the imported definition's: what its channels were called and
    // which mode they came from no longer describe them.
    fixture->labels.clear();
    fixture->profile = dmx::Fixture::ProfileLink{};
    commit();
}

void FixturesController::addChannel() {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    if (std::size_t{fixture->address} + fixture->channels.size() > dmx::kChannelsPerUniverse) {
        // Said, rather than a button that does nothing: the start address may have been typed
        // just now, and committed by this very click.
        setStatus("Channel " +
                      std::to_string(std::size_t{fixture->address} + fixture->channels.size()) +
                      " would be past the end of the universe.",
                  true);
        return;
    }
    fixture->channels.push_back(dmx::Role::Unused);
    fixture->parked.push_back(0);
    // A fixture with names for its channels keeps them beside their channels; the new one has
    // none.
    if (!fixture->labels.empty()) {
        fixture->labels.resize(fixture->channels.size());
    }
    commit();
}

void FixturesController::removeChannel(int index) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= fixture->channels.size()) {
        return;
    }
    fixture->channels.erase(fixture->channels.begin() + index);
    if (static_cast<std::size_t>(index) < fixture->parked.size()) {
        fixture->parked.erase(fixture->parked.begin() + index);
    }
    // And its name with it, or every name after it would describe the channel before.
    if (static_cast<std::size_t>(index) < fixture->labels.size()) {
        fixture->labels.erase(fixture->labels.begin() + index);
    }
    // Out of the rows' model at its own place, as a fixture is out of the list's.
    if (static_cast<std::size_t>(index) < channelModel_->row_count()) {
        channelModel_->erase(static_cast<std::size_t>(index));
    }
    commit();
}

void FixturesController::pickChannelRole(int index, int role) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= fixture->channels.size() || role < 0 ||
        static_cast<std::size_t>(role) >= dmx::kRoles.size()) {
        return;
    }
    fixture->channels[static_cast<std::size_t>(index)] =
        dmx::kRoles[static_cast<std::size_t>(role)];
    commit();
}

void FixturesController::setChannelParked(int index, int level) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= fixture->channels.size()) {
        return;
    }
    fixture->parked.resize(fixture->channels.size(), 0);
    const auto clamped = static_cast<std::uint8_t>(std::clamp(level, 0, 255));
    if (fixture->parked[static_cast<std::size_t>(index)] == clamped) {
        return; // a re-patch for nothing ends a running TEST
    }
    fixture->parked[static_cast<std::size_t>(index)] = clamped;
    commit();
}

void FixturesController::setPanRange(float low, float high) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    // Sorted rather than refused: an ordinary gesture, not an error to report.
    const double from = std::clamp(static_cast<double>(std::min(low, high)) / 100.0, 0.0, 1.0);
    const double to = std::clamp(static_cast<double>(std::max(low, high)) / 100.0, 0.0, 1.0);
    if (from == fixture->panMin && to == fixture->panMax) {
        return; // a re-patch for nothing ends a running TEST
    }
    fixture->panMin = from;
    fixture->panMax = to;
    commit();
}

void FixturesController::setTiltRange(float low, float high) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    const double from = std::clamp(static_cast<double>(std::min(low, high)) / 100.0, 0.0, 1.0);
    const double to = std::clamp(static_cast<double>(std::max(low, high)) / 100.0, 0.0, 1.0);
    if (from == fixture->tiltMin && to == fixture->tiltMax) {
        return;
    }
    fixture->tiltMin = from;
    fixture->tiltMax = to;
    commit();
}

void FixturesController::identify() {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr || refusedForPanic() || refusedForOff(*fixture, "identify")) {
        return;
    }
    if (static_cast<std::size_t>(selected_) >= dmx::kMaxRoutableFixtures) {
        // An effect is aimed by a bit per fixture, like a rule's, so this one cannot be flashed;
        // said, rather than a button that does nothing (the audit's M21). TEST reaches it.
        setStatus("identify reaches the first " + std::to_string(dmx::kMaxRoutableFixtures) +
                      " fixtures only; use a channel's test for this one.",
                  true);
        return;
    }
    dmx::FixtureSet mask;
    mask.set(static_cast<std::size_t>(selected_));

    // **A flash on every light-emitting channel the fixture actually has.**
    //
    // Flash is the shape that makes this safe to press during a set: it jumps to full and
    // decays back over the duration to wherever each channel was when it started
    // (`Payload::baseIsCurrent`, which the engine resolves), so it puts the fixture back where
    // it found it. Driving a color to white and leaving it there would identify the lamp and
    // then leave it white — which is how a lamp gets left on after an operator has walked back
    // from the truss. It used to decay to 0, which is where it found a *dark* fixture only: a
    // lit one went out, and an LED par lost its colour (the 2026-09-25 audit's L1).
    //
    // Every emitter rather than the dimmer alone, because plenty of pars have no dimmer at
    // all — **and the dimmer whether the fixture has one or not**: a par without one has its
    // colour scaled by an intensity of its own (`dmx::DmxEngine`), which a dimmer flash a rule
    // left at nothing kept dark through the colour flashes. Flashed, it is lit; decayed back,
    // it is where it was.
    for (const dmx::Role role :
         {dmx::Role::Dimmer, dmx::Role::Red, dmx::Role::Green, dmx::Role::Blue, dmx::Role::White}) {
        if (role != dmx::Role::Dimmer && !dmx::has(*fixture, role)) {
            continue;
        }
        dmx::Payload payload;
        payload.kind = dmx::EffectKind::Flash;
        payload.role = role;
        payload.level = 255;
        payload.baseIsCurrent = true;
        // Ease-in holds it near full for most of the time and drops away at the end, which is
        // what "look at me" wants — an ease-out would be gone before anybody looked up.
        payload.curve = dmx::Curve::EaseIn;
        payload.durationSeconds = static_cast<float>(kIdentifySeconds);
        runner_.post(output::OutputCommand::effect(mask, payload));
    }
    setStatus("Identifying " + fixture->name + "…", false);
}

bool FixturesController::refusedForOff(const dmx::Fixture& fixture, const char* verb) {
    // A fixture left out of the show is sent nothing — its universe let go of, when it was the
    // last on it — and IDENTIFY and TEST said they were lighting it all the same.
    if (fixture.enabled) {
        return false;
    }
    setStatus((fixture.name.empty() ? std::string("This fixture") : fixture.name) +
                  " is left out of the show, so nothing goes to it: tick \"in the show\" to " +
                  verb + " it.",
              true);
    return true;
}

bool FixturesController::refusedForPanic() {
    // The runner drops a hand-fired effect while PANIC is engaged (the audit's M17); this is
    // the window saying so, rather than a button that did nothing.
    if (!runner_.panicked()) {
        return false;
    }
    setStatus("PANIC is engaged, so nothing goes to the lights until it is released.", true);
    return true;
}

void FixturesController::setTestLevel(int level) {
    testLevel_ = std::clamp(level, 0, 255);
    window_->set_test_level(testLevel_);
}

void FixturesController::testChannel(int index) {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= fixture->channels.size() || refusedForPanic() ||
        refusedForOff(*fixture, "test")) {
        return;
    }
    // The real DMX number, which is the fixture's start address plus the offset — the number
    // on the row, on the back of the fixture, and in its manual. `channelOf` cannot be used:
    // it looks a *role* up, and two channels can carry one role while an unused channel
    // carries none. That is the whole reason this is addressed by index.
    const std::size_t number = std::size_t{fixture->address} + static_cast<std::size_t>(index);
    if (number < 1 || number > dmx::kChannelsPerUniverse) {
        setStatus("Channel " + std::to_string(number) + " is off the end of the universe.", true);
        return;
    }
    runner_.post(output::OutputCommand::channelTest(fixture->universe,
                                                    static_cast<std::uint16_t>(number),
                                                    static_cast<std::uint8_t>(testLevel_),
                                                    kTestSeconds));
    setStatus("Channel " + std::to_string(number) + " at " + std::to_string(testLevel_) + " for " +
                  std::to_string(static_cast<int>(kTestSeconds)) + " s.",
              false);
}

// --- the import --------------------------------------------------------------------------------

namespace {

/// "Elation · Rayzor 760": the maker, where the file names one, and the model as a list shows it.
std::string definitionName(const std::vector<fixtures::FixtureProfile>& library,
                           const fixtures::FixtureProfile& profile) {
    return (profile.manufacturer.empty() ? std::string() : profile.manufacturer + " · ") +
           fixtures::displayName(library, profile);
}

/// "a", "a and b", "a, b and c".
std::string listed(const std::vector<std::string>& names) {
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i) {
        text += i == 0 ? "" : i + 1 == names.size() ? " and " : ", ";
        text += names[i];
    }
    return text;
}

std::string counted(std::size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

constexpr const char* kFileHint = "a GDTF file (.gdtf) or an Open Fixture Library file (.json)";

} // namespace

const fixtures::FixtureProfile* FixturesController::importProfile() const noexcept {
    if (import_.askingAbout) {
        return nullptr; // nothing is picked while the question is open
    }
    if (import_.incoming) {
        return &*import_.incoming;
    }
    return fixtures::findProfile(library_, import_.entryId);
}

void FixturesController::openImport() {
    commitDrafts();
    if (import_.open) {
        return;
    }
    import_ = Import{};
    import_.open = true;
    // Where the block goes, to start with: the universe of the fixture showing — or of the last
    // one patched — and the first channel after everything already patched on it.
    const dmx::Fixture* const showing = current();
    import_.universe = showing != nullptr  ? showing->universe
                       : fixtures_.empty() ? dmx::PortAddress{0}
                                           : fixtures_.back().universe;
    int after = 0;
    for (const dmx::Fixture& fixture : fixtures_) {
        if (fixture.universe == import_.universe) {
            after = std::max<int>(after, dmx::lastChannelOf(fixture));
        }
    }
    import_.address = std::clamp(after + 1, 1, static_cast<int>(dmx::kChannelsPerUniverse));
    setStatus({}, false); // it was about the fixture the import now covers
    window_->set_import_type_folded(false);
    window_->set_import_mode_folded(false);
    window_->set_import_where_folded(false);
    publishImport();
}

void FixturesController::closeImport() {
    if (!import_.open) {
        return;
    }
    import_ = Import{};
    publishImport();
}

void FixturesController::forgetImportFile() {
    import_.incoming.reset();
    import_.askingAbout.reset();
    import_.fileLine.clear();
    import_.fileNote.clear();
    import_.fileError = false;
}

void FixturesController::pickFirstImportMode() {
    import_.mode.clear();
    if (const fixtures::FixtureProfile* const profile = importProfile()) {
        for (const fixtures::ProfileMode& mode : profile->modes) {
            if (mode.importable()) {
                import_.mode = mode.name;
                break;
            }
        }
    }
}

void FixturesController::pickLibraryEntry(int row) {
    if (!import_.open || row < 0 || static_cast<std::size_t>(row) >= libraryOrder_.size()) {
        return;
    }
    const fixtures::FixtureProfile& entry = library_[libraryOrder_[static_cast<std::size_t>(row)]];
    if (import_.entryId == entry.id && !import_.incoming && !import_.askingAbout) {
        return; // picked already: the mode chosen stays
    }
    forgetImportFile();
    import_.entryId = entry.id;
    pickFirstImportMode();
    publishImport();
}

void FixturesController::removeLibraryEntry(int row) {
    if (!import_.open || row < 0 || static_cast<std::size_t>(row) >= libraryOrder_.size()) {
        return;
    }
    const fixtures::FixtureProfile& entry = library_[libraryOrder_[static_cast<std::size_t>(row)]];
    const std::string id = entry.id;
    const std::string name = definitionName(library_, entry);
    // Only an entry no fixture is made from (decision 13): its × is not drawn otherwise, and
    // this is the belt to that.
    if (!fixtures::removeProfile(library_, id, fixtures_)) {
        setStatus(name + " has fixtures made from it, so it stays in the library.", true);
        publishImport();
        return;
    }
    if (import_.entryId == id) {
        import_.entryId.clear();
        import_.mode.clear();
    }
    // The question about it is moot: the file's definition is new to the library now.
    if (import_.askingAbout && *import_.askingAbout == id) {
        import_.askingAbout.reset();
        import_.fileNote = "new to this preset: added to its library when you import";
        pickFirstImportMode();
    }
    commitLibrary();
    setStatus(name + " is out of this preset's library.", false);
    publishImport();
}

void FixturesController::chooseImportFile() {
    if (!import_.open) {
        return;
    }
    fileDialogs_.open("Import a fixture's definition", FileKind::FixtureDefinition,
                      [this](const FileChoice& choice) {
                          if (!choice.problem.empty()) {
                              setStatus(choice.problem, true);
                              return;
                          }
                          if (!choice.path.empty()) { // empty is a cancel
                              importFile(choice.path);
                          }
                      });
}

void FixturesController::importFile(const std::filesystem::path& path) {
    if (!import_.open) {
        openImport();
    }
    forgetImportFile();
    import_.entryId.clear();
    import_.mode.clear();
    const std::string fileName = io::pathText(path.filename());
    std::string problem;
    std::optional<fixtures::FixtureProfile> read = fixtures::importProfile(path, problem);
    if (!read) {
        // Said where the file was asked for, in plain words (the operator, 2026-10-05).
        import_.fileLine = fileName;
        import_.fileNote = "Can't read " + fileName + ": " + problem + ".";
        import_.fileError = true;
        publishImport();
        return;
    }
    import_.fileLine = fileName + " · " +
                       (read->manufacturer.empty() ? std::string() : read->manufacturer + " · ") +
                       read->model;
    const fixtures::Comparison seen = fixtures::compareWithLibrary(library_, *read);
    switch (seen.arrival) {
    case fixtures::Arrival::New:
        import_.incoming = std::move(*read);
        import_.fileNote = "new to this preset: added to its library when you import";
        pickFirstImportMode();
        break;
    case fixtures::Arrival::Same:
        // The library has it, the same: that entry, and nothing added (decision 12).
        import_.entryId = library_[seen.index].id;
        import_.fileNote = "this preset's library has it already, the same — picked above";
        pickFirstImportMode();
        break;
    case fixtures::Arrival::Changed:
        // Another version of an entry: asked before anything changes.
        import_.incoming = std::move(*read);
        import_.askingAbout = library_[seen.index].id;
        break;
    }
    publishImport();
}

void FixturesController::answerReplace() {
    if (!import_.askingAbout || !import_.incoming) {
        return;
    }
    const std::string id = *import_.askingAbout;
    const auto at =
        std::find_if(library_.begin(), library_.end(),
                     [&id](const fixtures::FixtureProfile& one) { return one.id == id; });
    if (at == library_.end()) {
        answerCancel();
        return;
    }
    const std::string model = at->model;
    const std::size_t users = fixtures::usersOf(fixtures_, id);
    const std::vector<std::string> unlinked =
        fixtures::replaceProfile(library_, static_cast<std::size_t>(at - library_.begin()),
                                 std::move(*import_.incoming), fixtures_);
    import_.incoming.reset();
    import_.askingAbout.reset();
    import_.entryId = id;
    import_.fileNote = "the library's " + model + " is this file's now";
    pickFirstImportMode();
    commitLibrary();
    commit(); // the fixtures made from it, brought up to date
    std::string said = model + " replaced";
    if (users > 0) {
        said +=
            ": " + counted(users - unlinked.size(), "fixture", "fixtures") + " brought up to date";
    }
    if (!unlinked.empty()) {
        said += "; " + listed(unlinked) + (unlinked.size() == 1 ? " keeps its" : " keep their") +
                " channels but " + (unlinked.size() == 1 ? "is" : "are") + " no longer linked — " +
                (unlinked.size() == 1 ? "its" : "their") + " mode is not in the new file";
    }
    setStatus(said + ".", !unlinked.empty());
}

void FixturesController::answerKeepBoth() {
    if (!import_.askingAbout || !import_.incoming) {
        return;
    }
    const std::size_t index = fixtures::addProfile(library_, std::move(*import_.incoming));
    import_.incoming.reset();
    import_.askingAbout.reset();
    import_.entryId = library_[index].id;
    import_.fileNote = "kept both: this file's is the second entry, shown with its revision";
    pickFirstImportMode();
    commitLibrary();
    publishImport();
}

void FixturesController::answerCancel() {
    forgetImportFile();
    import_.entryId.clear();
    import_.mode.clear();
    publishImport();
}

void FixturesController::pickImportMode(int row) {
    const fixtures::FixtureProfile* const profile = importProfile();
    if (!import_.open || profile == nullptr || row < 0 ||
        static_cast<std::size_t>(row) >= profile->modes.size()) {
        return;
    }
    const fixtures::ProfileMode& mode = profile->modes[static_cast<std::size_t>(row)];
    if (!mode.importable()) {
        return; // it says why, and has no dot to pick
    }
    import_.mode = mode.name;
    publishImport();
}

void FixturesController::setImportCount(int count) {
    import_.count = std::clamp(count, 1, static_cast<int>(dmx::kChannelsPerUniverse));
    publishImport();
}

void FixturesController::setImportUniverse(const std::string& text) {
    dmx::PortAddress universe = 0;
    if (!dmx::parsePortAddress(text, universe)) {
        setStatus("A universe is a number from 0 to 32767, or net:sub:uni past the first 256.",
                  true);
        publishImport();
        return;
    }
    setStatus({}, false);
    import_.universe = universe;
    publishImport();
}

void FixturesController::setImportAddress(int address) {
    import_.address = std::clamp(address, 1, static_cast<int>(dmx::kChannelsPerUniverse));
    publishImport();
}

void FixturesController::confirmImport() {
    if (!import_.open) {
        return;
    }
    const fixtures::FixtureProfile* const profile = importProfile();
    if (profile == nullptr || import_.mode.empty()) {
        setStatus("Pick a fixture type and one of its modes first.", true);
        return;
    }
    // Checked before anything changes: a file's definition goes into the library only with
    // fixtures made from it.
    fixtures::NewFixtures made = fixtures::makeFixtures(
        *profile, import_.mode, import_.count, import_.universe, import_.address, fixtures_);
    if (!made.problem.empty() || made.fixtures.empty()) {
        setStatus("Nothing imported: " + made.problem + ".", true);
        publishImport();
        return;
    }
    const std::string model = profile->model;
    const std::string modeName = import_.mode;
    bool added = false;
    if (import_.incoming) {
        const std::size_t index = fixtures::addProfile(library_, std::move(*import_.incoming));
        import_.incoming.reset();
        import_.entryId = library_[index].id;
        added = true;
        // Made again from the library's copy, so they are linked to its id.
        made = fixtures::makeFixtures(library_[index], modeName, import_.count, import_.universe,
                                      import_.address, fixtures_);
        if (!made.problem.empty() || made.fixtures.empty()) {
            // The same arguments as the check above, so never — and if ever, the library keeps
            // the definition and nothing is patched.
            commitLibrary();
            setStatus("Nothing imported: " + made.problem + ".", true);
            publishImport();
            return;
        }
    }
    const bool pair = made.fixtures.size() == 2 * static_cast<std::size_t>(import_.count) &&
                      !made.fixtures.front().profile.pair.empty();
    const std::size_t first = fixtures_.size();
    for (dmx::Fixture& fixture : made.fixtures) {
        fixtures_.push_back(std::move(fixture));
    }
    selected_ = static_cast<int>(first);
    const dmx::PortAddress universe = import_.universe;
    const int count = import_.count;
    closeImport();
    if (added) {
        commitLibrary();
    }
    commit();
    std::string said =
        "Imported " +
        counted(static_cast<std::size_t>(count), pair ? "pair of " : "", pair ? "pairs of " : "") +
        model + " in " + modeName + ", channels " + std::to_string(made.firstChannel) + " to " +
        std::to_string(made.lastChannel) + " of universe " + dmx::describePortAddress(universe) +
        ".";
    if (fixtures_.size() > dmx::kMaxRoutableFixtures) {
        said += " Rules reach the first " + std::to_string(dmx::kMaxRoutableFixtures) +
                " fixtures only.";
    }
    setStatus(said, fixtures_.size() > dmx::kMaxRoutableFixtures);
}

void FixturesController::publishImport() {
    window_->set_importing(import_.open);

    // A: the library, by maker and model, each with how many fixtures are made from it.
    libraryOrder_ = fixtures::libraryOrder(library_);
    std::vector<LibraryRow> rows;
    rows.reserve(libraryOrder_.size());
    for (const std::size_t index : libraryOrder_) {
        const fixtures::FixtureProfile& entry = library_[index];
        const std::size_t users = fixtures::usersOf(fixtures_, entry.id);
        LibraryRow row{};
        row.maker = shared(entry.manufacturer.empty() ? std::string() : entry.manufacturer + " · ");
        row.model = shared(fixtures::displayName(library_, entry));
        row.users =
            shared(users == 0 ? std::string("no fixture") : counted(users, "fixture", "fixtures"));
        row.removable = users == 0;
        row.picked = !import_.incoming && !import_.askingAbout && import_.entryId == entry.id;
        rows.push_back(std::move(row));
    }
    writeRows(*libraryModel_, rows);

    const fixtures::FixtureProfile* const profile = importProfile();
    window_->set_import_file_line(shared(import_.fileLine.empty() ? kFileHint : import_.fileLine));
    // Under it, what the file is to the library — and anything the definition itself says.
    std::string note = import_.fileNote;
    if (profile != nullptr) {
        for (const std::string& line : profile->notes) {
            note += (note.empty() ? "" : "\n") + line;
        }
    }
    window_->set_import_file_note(shared(note));
    window_->set_import_file_error(import_.fileError);

    // The question, for a file the library has another version of.
    const fixtures::FixtureProfile* const asked =
        import_.askingAbout && import_.incoming
            ? fixtures::findProfile(library_, *import_.askingAbout)
            : nullptr;
    window_->set_import_asking(asked != nullptr);
    if (asked != nullptr) {
        const fixtures::FixtureProfile& incoming = *import_.incoming;
        std::size_t changed = 0;
        for (const fixtures::ProfileMode& mode : incoming.modes) {
            const fixtures::ProfileMode* const before = fixtures::findMode(*asked, mode.name);
            if (before == nullptr || !(*before == mode)) {
                ++changed;
            }
        }
        std::size_t gone = 0;
        for (const fixtures::ProfileMode& mode : asked->modes) {
            if (fixtures::findMode(incoming, mode.name) == nullptr) {
                ++gone;
            }
        }
        const auto revised = [](const fixtures::FixtureProfile& one) {
            return one.revision.empty() ? std::string("no revision given")
                                        : "revised " + one.revision;
        };
        std::vector<std::string> users;
        for (const dmx::Fixture& fixture : fixtures_) {
            if (fixture.profile.id == asked->id) {
                users.push_back(fixture.name);
            }
        }
        window_->set_ask_question(shared(asked->model +
                                         " is in this preset's library already, and this file is "
                                         "not the same."));
        window_->set_ask_library(
            shared(revised(*asked) + " · used by " +
                   (users.empty() ? std::string("no fixture")
                                  : counted(users.size(), "fixture", "fixtures"))));
        window_->set_ask_file(shared(revised(incoming) + " · " +
                                     counted(incoming.modes.size(), "mode", "modes") + ", " +
                                     std::to_string(changed) + " changed" +
                                     (gone > 0 ? ", " + std::to_string(gone) + " gone" : "")));
        window_->set_ask_replace(shared(
            users.empty()
                ? "the library's entry becomes this file's; no fixture is made from it"
                : "bring " +
                      (users.size() <= 3 ? listed(users)
                                         : counted(users.size(), "fixture", "fixtures")) +
                      " up to date — names, addresses and groups stay; one whose mode has gone "
                      "keeps its channels and is no longer linked"));
    }

    // B: the modes.
    window_->set_import_picked(profile != nullptr);
    window_->set_import_definition(
        shared(profile == nullptr ? std::string() : definitionName(library_, *profile)));
    std::vector<ModeRow> modes;
    if (profile != nullptr) {
        modes.reserve(profile->modes.size());
        for (const fixtures::ProfileMode& mode : profile->modes) {
            ModeRow row{};
            row.name = shared(mode.name);
            row.picked = mode.name == import_.mode;
            if (mode.importable()) {
                row.channels = shared(fixtures::channelCountOf(mode));
                row.drives = shared(fixtures::drivesOf(mode));
                row.unused = static_cast<int>(fixtures::unusedIn(mode));
            } else {
                row.refused =
                    shared(mode.refused.empty() ? std::string("it has no channels") : mode.refused);
            }
            std::string notes;
            for (const std::string& line : mode.notes) {
                notes += (notes.empty() ? "" : "\n") + line;
            }
            row.notes = shared(notes);
            modes.push_back(std::move(row));
        }
    }
    writeRows(*importModesModel_, modes);
    window_->set_import_mode(shared(import_.mode));

    // C: where, how many, what that takes and what they will be called.
    window_->set_import_count(import_.count);
    window_->set_import_universe(shared(dmx::describePortAddress(import_.universe)));
    window_->set_import_address(import_.address);
    std::string takes = "pick a mode above";
    bool takesError = false;
    std::string names;
    std::string overlap;
    bool pair = false;
    bool can = false;
    if (profile != nullptr && !import_.mode.empty()) {
        const fixtures::NewFixtures made = fixtures::makeFixtures(
            *profile, import_.mode, import_.count, import_.universe, import_.address, fixtures_);
        const fixtures::ProfileMode* const mode = fixtures::findMode(*profile, import_.mode);
        pair = mode != nullptr && mode->parts.size() == 2;
        if (!made.problem.empty() || made.fixtures.empty()) {
            takes = made.problem;
            takesError = true;
        } else {
            can = true;
            const std::string universe = dmx::describePortAddress(import_.universe);
            const std::string span = made.firstChannel == made.lastChannel
                                         ? "channel " + std::to_string(made.firstChannel)
                                         : "channels " + std::to_string(made.firstChannel) +
                                               " to " + std::to_string(made.lastChannel);
            if (pair && import_.count == 1) {
                takes = span + " on universe " + universe + ": part 1 at " +
                        std::to_string(made.fixtures[0].address) + ", part 2 at " +
                        std::to_string(made.fixtures[1].address);
            } else if (pair) {
                takes = span + " on universe " + universe + ", each copy part 1 then part 2";
            } else if (import_.count > 1) {
                takes = span + " on universe " + universe + ", " +
                        std::to_string(made.fixtures.front().channels.size()) + " each";
            } else {
                takes = span + " on universe " + universe;
            }
            std::vector<std::string> madeNames;
            for (const dmx::Fixture& fixture : made.fixtures) {
                madeNames.push_back(fixture.name);
            }
            names = madeNames.size() <= 4
                        ? listed(madeNames)
                        : madeNames.front() + ", " + madeNames[1] + " … " + madeNames.back() +
                              " (" + counted(madeNames.size(), "fixture", "fixtures") + ")";
            names += " · group " + made.fixtures.front().group;
            // Fixtures already on those channels: allowed — either can be re-addressed after —
            // and said, because two fixtures on one channel drive each other.
            std::vector<std::string> under;
            for (const dmx::Fixture& existing : fixtures_) {
                if (existing.universe != import_.universe || existing.channels.empty()) {
                    continue;
                }
                const int from = existing.address;
                const int to = dmx::lastChannelOf(existing);
                if (from <= made.lastChannel && to >= made.firstChannel) {
                    under.push_back(existing.name + " (" + whereOf(existing) + ")");
                }
            }
            if (!under.empty()) {
                overlap = "lands on " + under.front() +
                          (under.size() > 1 ? " and " + counted(under.size() - 1, "more", "more")
                                            : std::string()) +
                          " — import anyway and re-address one of them after, or start later";
            }
        }
    }
    window_->set_import_takes(shared(takes));
    window_->set_import_takes_error(takesError);
    window_->set_import_names(shared(names));
    window_->set_import_overlap(shared(overlap));
    window_->set_import_pair(pair && can);
    window_->set_import_can(can);
}

} // namespace takt4::ui
