// `RulesController`, the Liberation preset's prompt: how many lasers, which clips, where Liberation
// is and where its zones go — answered here, worked out by `rule_presets::planLiberation`, and
// added by `addLiberation`. The rest of the editor is in `rules_controller.cpp`.

#include "core/dmx/artnet_packet.hpp"
#include "core/dmx/liberation.hpp"
#include "ui/model_rows.hpp"
#include "ui/rule_presets.hpp"
#include "ui/rules_controller.hpp"
#include "ui/rules_editor_support.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace takt4::ui {

using namespace rule_presets;
using namespace rules_detail;

namespace {

std::string trimmed(std::string text) {
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    return text;
}

} // namespace

void RulesController::openLiberation() {
    liberationOpen_ = true;
    publishLiberation();
    window_->set_liberation_open(true);
}

void RulesController::closeLiberation() {
    liberationOpen_ = false;
    window_->set_liberation_open(false);
}

void RulesController::setLaserCount(int lasers) {
    liberationAsk_.lasers = std::clamp(lasers, 1, kMaxLasers);
    publishLiberation();
}

void RulesController::setLaserClip(int laser, bool to, const std::string& text) {
    if (laser < 0 || laser >= kMaxLasers) {
        return;
    }
    // Kept as typed, so the box shows what was typed — and spelled the way Liberation spells a clip
    // once it reads as one, so "21 1" comes back as 21-1.
    std::string kept = trimmed(text);
    if (const std::optional<dmx::liberation::Clip> clip = dmx::liberation::parseClip(kept)) {
        kept = dmx::liberation::format(*clip);
    }
    (to ? liberationAsk_.to : liberationAsk_.from)[static_cast<std::size_t>(laser)] = kept;
    publishLiberation();
}

void RulesController::setLaserTiming(int laser, int timing) {
    if (laser < 0 || laser >= kMaxLasers || timing < 0 ||
        static_cast<std::size_t>(timing) >= kLaserTimings.size()) {
        return;
    }
    liberationAsk_.timing[static_cast<std::size_t>(laser)] = timing;
    publishLiberation();
}

void RulesController::setLiberationHost(const std::string& host) {
    liberationAsk_.host = trimmed(host);
    publishLiberation();
}

void RulesController::setLiberationPort(int port) {
    liberationAsk_.port = static_cast<std::uint16_t>(std::clamp(port, 1, 65535));
    publishLiberation();
}

void RulesController::setLiberationUniverse(int universe) {
    liberationAsk_.universe = std::clamp(universe, 1, static_cast<int>(dmx::kMaxPortAddress) + 1);
    publishLiberation();
}

void RulesController::setLiberationAddress(int address) {
    liberationAsk_.address = std::clamp(address, 1, static_cast<int>(dmx::kChannelsPerUniverse));
    publishLiberation();
}

void RulesController::setLiberationMove(bool on) {
    liberationAsk_.move = on;
    publishLiberation();
}

void RulesController::setLiberationShape(int shape) {
    if (shape < 0 || static_cast<std::size_t>(shape) >= kLaserMoves.size()) {
        return;
    }
    liberationAsk_.moveShape = shape;
    publishLiberation();
}

void RulesController::setLiberationAmount(int amount) {
    liberationAsk_.amount = std::clamp(amount, 1, 100);
    publishLiberation();
}

void RulesController::setLiberationBars(int bars) {
    liberationAsk_.bars = std::clamp(bars, 1, 64);
    publishLiberation();
}

void RulesController::publishLiberation() {
    const LiberationAsk& ask = liberationAsk_;
    const LiberationPlan plan = planLiberation(ask, patch_);
    std::vector<LaserRow> rows;
    for (int laser = 0; laser < ask.lasers; ++laser) {
        const auto at = static_cast<std::size_t>(laser);
        LaserRow row{};
        row.label = shared("laser " + std::to_string(laser + 1));
        row.from = shared(ask.from[at]);
        row.to = shared(ask.to[at]);
        row.clips = shared(plan.clips[at]);
        row.timing = ask.timing[at];
        row.where = shared(plan.where[at]);
        rows.push_back(std::move(row));
    }
    // In place: a row whose box is being typed in keeps its element (`writeRows`).
    laserRows_.write(rows);
    window_->set_laser_count(ask.lasers);
    window_->set_liberation_host(shared(ask.host));
    window_->set_liberation_port(static_cast<int>(ask.port));
    window_->set_liberation_universe(ask.universe);
    window_->set_liberation_address(ask.address);
    window_->set_liberation_takt4_universe(
        shared("→ takt4's patch editor calls it universe " +
               dmx::describePortAddress(dmx::liberation::portAddressOf(ask.universe))));
    window_->set_liberation_move(ask.move);
    window_->set_liberation_shape(ask.moveShape);
    window_->set_liberation_amount(ask.amount);
    window_->set_liberation_bars(ask.bars);
    window_->set_liberation_instructions(shared(plan.instructions));
    window_->set_liberation_problem(shared(plan.problem));
}

void RulesController::addLiberation() {
    LiberationPlan plan = planLiberation(liberationAsk_, patch_);
    if (!plan.problem.empty()) {
        publishLiberation(); // the problem is on the prompt already; nothing is added
        return;
    }
    // The zones, the output and Link first, by whoever owns them — so that the rules, when they
    // arrive, aim at fixtures the runner already has. The patch comes back through `setPatch`.
    if (rigNeeded_) {
        rigNeeded_(RigSetup{plan.patch, trimmed(liberationAsk_.host), liberationAsk_.port});
    } else {
        setPatch(plan.patch);
    }
    closeLiberation();
    const int lasers = std::clamp(liberationAsk_.lasers, 1, kMaxLasers);
    const int reused = plan.reused;
    appendRig(std::move(plan.rules));
    const std::string count = std::to_string(lasers) + (lasers == 1 ? " laser" : " lasers");
    setStatus("Added " + count +
                  (reused > 0 ? " (" + std::to_string(reused) +
                                    " of the zones were "
                                    "in the patch already)"
                              : "") +
                  ". Nothing lights until Liberation has its DMX Input on with matching profiles, "
                  "and takt4 has a lock.",
              false);
}

} // namespace takt4::ui
