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

OscControl::OscControl(engine::BeatEngine& engine, Config config)
    : engine_(engine), config_(std::move(config)) {}

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
    started_ = std::chrono::steady_clock::now();
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

std::string OscControl::lastMessage() const {
    const std::lock_guard<std::mutex> lock(lastMutex_);
    return last_;
}

bool OscControl::dispatch(std::string_view address, std::optional<double> argument) {
    using engine::Command;

    // Everything §5.7 lists hangs off `<prefix>/ctl/`. A message for another app's
    // namespace is not ours to act on, however familiar the tail looks.
    const std::string base = config_.prefix + "/ctl/";
    if (address.size() <= base.size() || address.compare(0, base.size(), base) != 0) {
        return false;
    }
    const std::string_view verb = address.substr(base.size());

    if (verb == "tap") {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        const std::optional<double> tapped = taps_.tap(seconds);
        if (tapped) {
            // A seed, not an override — §7's locked decision, and the same thing the
            // window's TAP does.
            (void)engine_.post(Command::seedTempo(*tapped));
        }
        return true;
    }
    if (verb == "downbeat") {
        (void)engine_.post(Command::snapDownbeat());
        return true;
    }
    if (verb == "tempo/halve") {
        (void)engine_.post(Command::halve());
        return true;
    }
    if (verb == "tempo/double") {
        (void)engine_.post(Command::redouble());
        return true;
    }
    (void)argument; // no address takes one yet; /ctl/lock is where that starts
    return false;
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
