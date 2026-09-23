#include "ui/fixtures_controller.hpp"

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/effect.hpp"
#include "core/io/utf8.hpp"
#include "ui/window_state.hpp"

#include <algorithm>
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
/// Unique because a rule aims at a *name*: two fixtures called the same thing is not an error
/// (`resolveFixtures` takes both, which is the forgiving reading) but it is never what an
/// operator pressing ADD twice meant.
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
                                       std::vector<dmx::Fixture> fixtures)
    : runner_(runner), fixtures_(std::move(fixtures)), window_(FixturesWindow::create()),
      listModel_(std::make_shared<slint::VectorModel<FixtureRow>>()),
      channelModel_(std::make_shared<slint::VectorModel<ChannelRow>>()) {
    window_->set_fixtures(listModel_);
    window_->set_channels(channelModel_);

    auto roles = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Role role : dmx::kRoles) {
        roles->push_back(shared(std::string(dmx::labelOf(role))));
    }
    window_->set_roles(roles);

    auto modes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    // Index 0 is "custom", so that a channel map the operator has edited has something
    // honest to be — and so picking nothing is a state rather than a fixture nobody meant.
    modes->push_back(shared("custom"));
    for (const dmx::FixtureMode& mode : dmx::builtinModes()) {
        modes->push_back(shared(std::string(mode.name)));
    }
    window_->set_modes(modes);

    window_->on_picked([this](int index) { pick(index); });
    window_->on_added([this] { add(); });
    window_->on_duplicated([this] { duplicate(); });
    window_->on_removed([this] { remove(); });
    window_->on_removed_at([this](int index) { removeAt(index); });
    window_->on_enabled_changed([this](int index, bool on) { setEnabledAt(index, on); });
    window_->on_name_edited([this](const slint::SharedString& text) { rename(std::string(text)); });
    window_->on_group_edited(
        [this](const slint::SharedString& text) { setGroup(std::string(text)); });
    window_->on_universe_edited(
        [this](const slint::SharedString& text) { setUniverse(std::string(text)); });
    window_->on_address_changed([this](int address) { setAddress(address); });
    window_->on_fixture_enabled_changed([this](bool on) { setEnabled(on); });
    window_->on_mode_picked([this](int mode) { pickMode(mode); });
    window_->on_channel_added([this] { addChannel(); });
    window_->on_channel_removed([this](int index) { removeChannel(index); });
    window_->on_channel_role_picked([this](int index, int role) { pickChannelRole(index, role); });
    window_->on_channel_parked_changed(
        [this](int index, int level) { setChannelParked(index, level); });
    window_->on_pan_range_changed([this](float low, float high) { setPanRange(low, high); });
    window_->on_tilt_range_changed([this](float low, float high) { setTiltRange(low, high); });
    window_->on_identify([this] { identify(); });
    window_->on_channel_tested([this](int index) { testChannel(index); });
    window_->on_test_level_changed([this](int level) { setTestLevel(level); });

    window_->set_test_level(testLevel_);
    window_->set_test_seconds(static_cast<int>(kTestSeconds));

    // What the window opens at. **Set from C++ and not from the markup**, for the reason
    // `ui::kRulesWindowWidth` gives at length: Slint sizes a window from what its content
    // asks for, so a `preferred-width` in the markup decides nothing and this window opened
    // at its 820x520 minimum — a patch editor two thirds the size of the rule editor beside
    // it. Asked for on a rig on 2026-09-16, measured from the shot they sent.
    window_->window().set_size(
        slint::LogicalSize({kFixturesWindowWidth, kFixturesWindowHeight}));

    resettle(fixtures_.empty() ? -1 : 0);
    publishAll();
    rebuildChannels();
}

void FixturesController::show() {
    visible_ = true;
    window_->show();
    publishAll();
    // Directly rather than through `tick`, because nothing is being destroyed from inside its
    // own handler here and a window that opened a frame empty would flicker.
    rebuildChannels();
}

void FixturesController::hide() {
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

void FixturesController::setFixtures(std::vector<dmx::Fixture> fixtures) {
    fixtures_ = std::move(fixtures);
    resettle(selected_);
    publishAll();
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
    window_->set_summary(shared(summary()));
}

void FixturesController::publishList() {
    std::vector<FixtureRow> rows;
    rows.reserve(fixtures_.size());
    for (const dmx::Fixture& fixture : fixtures_) {
        FixtureRow row{};
        row.name = shared(fixture.name);
        row.where = shared(whereOf(fixture));
        row.group = shared(fixture.group);
        row.enabled = fixture.enabled;
        row.problem = shared(dmx::problemWith(fixture));
        rows.push_back(std::move(row));
    }
    listModel_->set_vector(std::move(rows));
    window_->set_selected(selected_);
}

void FixturesController::publishSelected() {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        window_->set_name(shared({}));
        window_->set_group(shared({}));
        window_->set_universe(shared({}));
        window_->set_address(1);
        window_->set_enabled(true);
        window_->set_mode_index(0);
        window_->set_moves(false);
        return;
    }
    window_->set_name(shared(fixture->name));
    window_->set_group(shared(fixture->group));
    window_->set_universe(shared(dmx::describePortAddress(fixture->universe)));
    window_->set_address(static_cast<int>(fixture->address));
    window_->set_enabled(fixture->enabled);
    window_->set_mode_index(modeOf(*fixture));
    // The movement window is shown for a fixture that can move, and hidden for one that
    // cannot — on a wash the four numbers mean nothing and would be four controls an operator
    // has to work out are irrelevant.
    window_->set_moves(dmx::has(*fixture, dmx::Role::Pan) || dmx::has(*fixture, dmx::Role::Tilt));
    window_->set_pan_min(static_cast<float>(fixture->panMin * 100.0));
    window_->set_pan_max(static_cast<float>(fixture->panMax * 100.0));
    window_->set_tilt_min(static_cast<float>(fixture->tiltMin * 100.0));
    window_->set_tilt_max(static_cast<float>(fixture->tiltMax * 100.0));
}

ChannelRow FixturesController::rowFor(const dmx::Fixture& fixture, std::size_t index) const {
    ChannelRow row{};
    row.number = static_cast<int>(fixture.address) + static_cast<int>(index);
    row.role_index = 0;
    for (std::size_t r = 0; r < dmx::kRoles.size(); ++r) {
        if (dmx::kRoles[r] == fixture.channels[index]) {
            row.role_index = static_cast<int>(r);
            break;
        }
    }
    row.parked = index < fixture.parked.size() ? static_cast<int>(fixture.parked[index]) : 0;
    row.live = 0;
    return row;
}

void FixturesController::publishChannels() {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        if (channelModel_->row_count() != 0) {
            channelModel_->set_vector({});
        }
        channelsBuiltFor_ = -1;
        channelsShown_ = 0;
        return;
    }

    // **Updated in place while the shape is the same, and rebuilt only when it is not.**
    //
    // Rebuilding destroys every box in the repeater and makes new ones — and this runs
    // *inside* the callback of the very box being typed in, so a rebuild on every keystroke
    // would destroy an element from within its own handler and leave what was typed in a box
    // that no longer exists. `RulesController::publishSlots` documents the same trap and the
    // same answer; the deferral is what keeps it out of the handler.
    if (channelsBuiltFor_ == selected_ && channelsShown_ == fixture->channels.size()) {
        for (std::size_t i = 0; i < fixture->channels.size(); ++i) {
            ChannelRow row = rowFor(*fixture, i);
            const ChannelRow current = *channelModel_->row_data(i);
            row.live = current.live; // the mirror's, not this function's
            if (row.number != current.number || row.role_index != current.role_index ||
                row.parked != current.parked) {
                channelModel_->set_row_data(i, row);
            }
        }
        publishLevels();
        return;
    }
    channelsDirty_ = true;
}

void FixturesController::rebuildChannels() {
    const dmx::Fixture* const fixture = current();
    channelsDirty_ = false;
    if (fixture == nullptr) {
        channelModel_->set_vector({});
        channelsBuiltFor_ = -1;
        channelsShown_ = 0;
        return;
    }
    std::vector<ChannelRow> rows;
    rows.reserve(fixture->channels.size());
    for (std::size_t i = 0; i < fixture->channels.size(); ++i) {
        rows.push_back(rowFor(*fixture, i));
    }
    channelModel_->set_vector(std::move(rows));
    channelsBuiltFor_ = selected_;
    channelsShown_ = fixture->channels.size();
    publishLevels();
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
    if (channelsDirty_) {
        // One redraw later, which nobody sees, and it means no element is ever destroyed from
        // within its own handler. See `publishChannels`.
        rebuildChannels();
    }
    publishLevels();
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
    return text;
}

void FixturesController::setStatus(const std::string& text, bool error) {
    status_ = text;
    window_->set_status(shared(text));
    window_->set_status_error(error);
}

void FixturesController::pick(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixtures_.size()) {
        return;
    }
    selected_ = index;
    publishList();
    publishSelected();
    publishChannels();
}

void FixturesController::add() {
    if (fixtures_.size() >= dmx::kMaxRoutableFixtures) {
        // Past this a fixture can still be patched and driven by its own rules; what it
        // cannot be is *named* by one, because a rule carries its fixtures as a bit each.
        // Said out loud rather than discovered — see `dmx::kMaxRoutableFixtures`.
        setStatus("64 fixtures is as many as a rule can aim at by name. Add more only if you "
                  "are driving them from rules that already exist.",
                  true);
    }
    // Patched after the last one on its universe rather than at 1, because that is what an
    // operator adding a second par means and it is the arithmetic they would otherwise do by
    // hand. A universe with nothing on it starts at 1.
    dmx::Fixture fixture = dmx::fixtureFromMode(freshName(fixtures_, "fixture"), 1, 0, 1);
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
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    dmx::Fixture copy = *fixture;
    copy.name = freshName(fixtures_, fixture->name);
    // Addressed after the one it came from, which is what duplicating a par in a row means.
    const std::uint16_t after = dmx::lastChannelOf(*fixture);
    copy.address = static_cast<std::uint16_t>(
        std::min<int>(after + 1, static_cast<int>(dmx::kChannelsPerUniverse)));
    fixtures_.insert(fixtures_.begin() + selected_ + 1, std::move(copy));
    resettle(selected_ + 1);
    commit();
}

void FixturesController::remove() {
    if (current() == nullptr) {
        return;
    }
    fixtures_.erase(fixtures_.begin() + selected_);
    resettle(selected_);
    commit();
}

void FixturesController::removeAt(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixtures_.size()) {
        return;
    }
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
    // **A rule aims at a name, so renaming a fixture un-routes every rule that named it.**
    // Said rather than silently done: the rule keeps the old name (a preset opened on another
    // rig should, see `DmxSend::fixtures`), so plugging the name back in restores the routing.
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
    if (fixture == nullptr || mode <= 0) {
        return; // 0 is "custom", which describes the map rather than setting one
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
    commit();
}

void FixturesController::addChannel() {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr ||
        std::size_t{fixture->address} + fixture->channels.size() > dmx::kChannelsPerUniverse) {
        return;
    }
    fixture->channels.push_back(dmx::Role::Unused);
    fixture->parked.push_back(0);
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
    fixture->parked[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>(std::clamp(level, 0, 255));
    commit();
}

void FixturesController::setPanRange(float low, float high) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    // Sorted rather than refused: the two sliders are independent and dragging one past the
    // other is an ordinary gesture, not an error to report.
    fixture->panMin = std::clamp(std::min(low, high) / 100.0, 0.0, 1.0);
    fixture->panMax = std::clamp(std::max(low, high) / 100.0, 0.0, 1.0);
    commit();
}

void FixturesController::setTiltRange(float low, float high) {
    dmx::Fixture* const fixture = current();
    if (fixture == nullptr) {
        return;
    }
    fixture->tiltMin = std::clamp(std::min(low, high) / 100.0, 0.0, 1.0);
    fixture->tiltMax = std::clamp(std::max(low, high) / 100.0, 0.0, 1.0);
    commit();
}

void FixturesController::identify() {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr || static_cast<std::size_t>(selected_) >= dmx::kMaxRoutableFixtures) {
        return;
    }
    const std::uint64_t mask = std::uint64_t{1} << static_cast<std::size_t>(selected_);

    // **A flash on every light-emitting channel the fixture actually has.**
    //
    // Flash is the shape that makes this safe to press during a set: it jumps to full and
    // decays back to zero over the duration, so it puts the fixture back where it found it
    // without anything having to remember where that was. Driving a color to white and
    // leaving it there would identify the lamp and then leave it white — which is how a lamp
    // gets left on after an operator has walked back from the truss.
    //
    // Every emitter rather than the dimmer alone, because plenty of pars have no dimmer at
    // all: on those, a flash aimed at `Dimmer` reaches nothing and the lamp never lights.
    for (const dmx::Role role :
         {dmx::Role::Dimmer, dmx::Role::Red, dmx::Role::Green, dmx::Role::Blue, dmx::Role::White}) {
        if (!dmx::has(*fixture, role)) {
            continue;
        }
        dmx::Payload payload;
        payload.kind = dmx::EffectKind::Flash;
        payload.role = role;
        payload.level = 255;
        payload.base = 0;
        // Ease-in holds it near full for most of the time and drops away at the end, which is
        // what "look at me" wants — an ease-out would be gone before anybody looked up.
        payload.curve = dmx::Curve::EaseIn;
        payload.durationSeconds = static_cast<float>(kIdentifySeconds);
        runner_.post(output::OutputCommand::effect(mask, payload));
    }
    setStatus("Identifying " + fixture->name + "…", false);
}

void FixturesController::setTestLevel(int level) {
    testLevel_ = std::clamp(level, 0, 255);
    window_->set_test_level(testLevel_);
}

void FixturesController::testChannel(int index) {
    const dmx::Fixture* const fixture = current();
    if (fixture == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= fixture->channels.size()) {
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

} // namespace takt4::ui
