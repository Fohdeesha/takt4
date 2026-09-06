#include "core/control/osc_control.hpp"

#include "core/control/osc_parse.hpp"
#include "core/engine/control.hpp"

#include <exception>
#include <utility>

namespace takt4::control {
namespace {

/// How long the thread waits on the socket before looking at `running_` again. Long
/// enough that an idle receiver costs nothing, short enough that stopping is not a wait.
constexpr std::chrono::milliseconds kPoll{100};

} // namespace

OscControl::OscControl(engine::BeatEngine& engine, Config config, RuleControl* rules)
    : config_(std::move(config)), surface_(engine, rules) {}

OscControl::~OscControl() {
    stop();
}

void OscControl::start() {
    if (!config_.enabled || running()) {
        return;
    }
    // Before the thread, so a port that is taken throws to the caller rather than
    // disappearing into a worker nobody is watching.
    receiver_ = std::make_unique<OscReceiver>(config_.port, config_.localOnly);
    // A set of taps does not span a stop. `TapTempo` would drop a stale one on its own
    // timeout anyway; saying so here means it does not depend on how long the stop was.
    surface_.resetTaps();
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
}

void OscControl::stop() noexcept {
    if (!worker_.joinable()) {
        receiver_.reset();
        return;
    }
    running_.store(false, std::memory_order_release);
    // The thread wakes from its own timeout within kPoll and sees the flag; closing the
    // socket under it is not needed and would race with the recvfrom.
    worker_.join();
    receiver_.reset();
}

void OscControl::setConfig(Config config) {
    stop();
    config_ = std::move(config);
}

std::string OscControl::lastMessage() const {
    const std::lock_guard<std::mutex> lock(lastMutex_);
    return last_;
}

bool OscControl::dispatch(std::string_view address, std::optional<double> argument) {
    // Everything §5.7 lists hangs off `<prefix>/ctl/`. A message for another app's
    // namespace is not ours to act on, however familiar the tail looks.
    const std::string base = config_.prefix + "/ctl/";
    if (address.size() <= base.size() || address.compare(0, base.size(), base) != 0) {
        return false;
    }

    // Which leaves the verb, which is the whole of what this surface knows. What each one
    // does — and that `lock` refuses to act without its `<0|1>` — is `ControlSurface`'s,
    // shared with the MIDI bindings so the two surfaces cannot drift apart. `rule/<id>/enable`
    // is the one verb that carries a name as well; `targetOf` is what splits it out.
    const std::optional<ControlTarget> target = targetOf(address.substr(base.size()));
    if (!target) {
        return false;
    }
    return surface_.apply(*target, argument);
}

void OscControl::run() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        const std::span<const std::byte> datagram = receiver_->receive(kPoll);
        if (datagram.empty()) {
            continue;
        }
        const std::optional<OscView> message = parseOsc(datagram);
        if (!message) {
            // Not an OSC message at all. Counted, because a control surface pointed at
            // the wrong port looks exactly like a broken one otherwise.
            ignored_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        bool acted = false;
        try {
            acted = dispatch(message->address(), message->number(0));
        } catch (...) {
            // Nothing in dispatch throws today, and a control surface must not be able to
            // take the process down by sending something unexpected if one ever does.
            acted = false;
        }

        if (acted) {
            handled_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ignored_.fetch_add(1, std::memory_order_relaxed);
        }
        const std::lock_guard<std::mutex> lock(lastMutex_);
        last_ = std::string(message->address()) + (acted ? "" : "  (not understood)") + "  from " +
                receiver_->lastSender();
    }
}

} // namespace takt4::control
