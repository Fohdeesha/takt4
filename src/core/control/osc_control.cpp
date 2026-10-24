#include "core/control/osc_control.hpp"

#include "core/control/osc_parse.hpp"
#include "core/engine/control.hpp"
#include "core/remote/status_messages.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace takt4::control {
namespace {

/// How long the thread waits on the socket before looking at `running_` again. Long
/// enough that an idle receiver costs nothing, short enough that stopping is not a wait.
constexpr std::chrono::milliseconds kPoll{100};

} // namespace

OscControl::OscControl(engine::BeatSource& beats, Config config, RuleControl* rules)
    : config_(std::move(config)), surface_(beats, rules) {}

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
    // Every control address hangs off `<prefix>/ctl/`. A message for another app's
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

bool OscControl::dispatchRemote(std::span<const std::byte> packet, const remote::Endpoint& from) {
    const std::optional<OscView> message = parseOsc(packet);
    return message && handleRemote(*message, from);
}

void OscControl::handle(std::span<const std::byte> packet) noexcept {
    const std::optional<OscView> message = parseOsc(packet);
    if (!message) {
        // Not an OSC message at all. Counted, because a control surface pointed at
        // the wrong port looks exactly like a broken one otherwise.
        ignored_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    bool acted = false;
    bool remote = false;
    try {
        remote = handleRemote(*message, receiver_->lastFrom());
        acted = remote || dispatch(message->address(), message->number(0));
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
    try {
        const std::lock_guard<std::mutex> lock(lastMutex_);
        last_ = std::string(message->address()) + (acted ? "" : "  (not understood)") + "  from " +
                receiver_->lastSender();
    } catch (...) {
        // No memory for the line: the counts above still say it came.
    }
}

void OscControl::run() noexcept {
    while (running_.load(std::memory_order_acquire)) {
        const std::span<const std::byte> datagram = receiver_->receive(kPoll);
        if (datagram.empty()) {
            continue;
        }
        // A bundle's messages each as it was sent alone, at once (`forEachOscMessage`).
        std::size_t messages = 0;
        try {
            messages = forEachOscMessage(
                datagram, [this](std::span<const std::byte> packet) { handle(packet); });
        } catch (...) {
            messages = 0;
        }
        if (messages == 0 && isOscBundle(datagram)) {
            // A bundle with nothing readable in it: said, where a bundle used to be counted as
            // ignored with no line to say why.
            ignored_.fetch_add(1, std::memory_order_relaxed);
            try {
                const std::lock_guard<std::mutex> lock(lastMutex_);
                last_ =
                    "#bundle with no message takt4 can read in it  from " + receiver_->lastSender();
            } catch (...) {
            }
        }
    }
}

void OscControl::setRemoteSetup(RemoteSetup setup) {
    const std::lock_guard<std::mutex> lock(setupMutex_);
    setup_ = std::move(setup);
}

OscControl::RemoteSetup OscControl::remoteSetup() const {
    const std::lock_guard<std::mutex> lock(setupMutex_);
    return setup_;
}

bool OscControl::reply(const remote::Endpoint& to, std::span<const std::byte> packet) noexcept {
    if (!receiver_) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(replyMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (replyRefilled_ != std::chrono::steady_clock::time_point{}) {
            const double seconds = std::chrono::duration<double>(now - replyRefilled_).count();
            replyTokens_ = std::min(kRepliesPerSecond, replyTokens_ + seconds * kRepliesPerSecond);
        }
        replyRefilled_ = now;
        if (replyTokens_ < 1.0) {
            repliesHeld_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        replyTokens_ -= 1.0;
    }
    return receiver_->sendTo(to, packet);
}

void OscControl::answer(const remote::Command& command, bool ok, std::string_view note) {
    if (command.seq) {
        const remote::Packet packet = remote::replyMessage(config_.prefix, *command.seq, ok, note);
        (void)reply(command.from, packet);
    } else if (!ok) {
        const remote::Packet packet = remote::refusedMessage(config_.prefix, command.address, note);
        (void)reply(command.from, packet);
    }
}

void OscControl::refuse(const remote::Command& command, std::string_view why) {
    remoteRefused_.fetch_add(1, std::memory_order_relaxed);
    answer(command, false, why);
}

namespace {

/// The argument at `index` as a string, when it is one.
std::optional<std::string_view> textAt(const OscView& message, std::size_t index) {
    if (index >= message.argumentCount() ||
        message.argument(index).type != OscArgument::Type::String) {
        return std::nullopt;
    }
    return message.argument(index).asString;
}

/// The argument at `index` as a whole number, when it is an int — or a float, or a boolean, with
/// no fraction to lose.
std::optional<std::int32_t> wholeAt(const OscView& message, std::size_t index) {
    if (index >= message.argumentCount()) {
        return std::nullopt;
    }
    const std::optional<double> value = message.number(index);
    if (!value || !std::isfinite(*value) || std::floor(*value) != *value ||
        std::abs(*value) > 2147483647.0) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>(*value);
}

} // namespace

bool OscControl::handleRemote(const OscView& message, const remote::Endpoint& from) {
    const std::string base = config_.prefix + "/ctl/";
    const std::string_view address = message.address();
    if (address.size() <= base.size() || address.compare(0, base.size(), base) != 0) {
        return false;
    }
    const std::string_view verb = address.substr(base.size());
    const std::size_t count = message.argumentCount();

    // A request number, as the last of `defined` arguments plus one: then the sender is answered
    // whatever the answer. With no arguments defined, **0 is a button let go of**, and does
    // nothing, as takt4's other buttons take it: a push button sends 1 and then 0.
    remote::Command command;
    command.address = std::string(address);
    command.from = from;
    bool released = false;
    const auto numbered = [&](std::size_t defined) {
        if (count == defined + 1) {
            const std::optional<std::int32_t> seq = wholeAt(message, defined);
            if (defined == 0 && seq && *seq == 0) {
                released = true;
            } else if (seq) {
                command.seq = *seq;
            }
        }
    };

    // --- what takt4 says about itself: any sender, no switch, changing nothing ---------------
    const bool status = verb == "status";
    const bool devices = verb == "audio/devices";
    const bool outputs = verb == "outputs";
    if (status || devices || outputs) {
        numbered(0);
        if (released) {
            return true;
        }
        if (board_ == nullptr) {
            refuse(command, "takt4 has no status to give here");
            return true;
        }
        remote::StatusBoard::Clock::time_point at{};
        const remote::Status now = board_->read(&at);
        std::vector<remote::Packet> packets;
        if (status) {
            const auto age = at == remote::StatusBoard::Clock::time_point{}
                                 ? std::int64_t{0}
                                 : std::chrono::duration_cast<std::chrono::milliseconds>(
                                       remote::StatusBoard::Clock::now() - at)
                                       .count();
            // One bundle, so what it says is one moment's: the snapshot's own shape.
            packets.push_back(remote::bundleOf(
                remote::statusMessages(config_.prefix, now, now.uptimeSeconds, age)));
        } else {
            packets = devices ? remote::deviceMessages(config_.prefix, now)
                              : remote::outputMessages(config_.prefix, now);
        }
        for (const remote::Packet& packet : packets) {
            (void)reply(from, packet);
        }
        queriesAnswered_.fetch_add(1, std::memory_order_relaxed);
        if (command.seq) {
            answer(command, true, "");
        }
        return true;
    }

    // --- what changes takt4: carried out by its window ----------------------------------------
    using Kind = remote::Command::Kind;
    bool known = true;
    bool wellFormed = true;
    if (verb == "audio/start" || verb == "audio/stop" || verb == "audio/restart") {
        command.kind = verb == "audio/start"  ? Kind::Start
                       : verb == "audio/stop" ? Kind::Stop
                                              : Kind::Restart;
        wellFormed = count <= 1;
        numbered(0);
        if (released) {
            return true;
        }
    } else if (verb == "audio/device") {
        command.kind = Kind::Device;
        const auto host = textAt(message, 0);
        const auto device = textAt(message, 1);
        wellFormed = host && device && (count == 2 || count == 3);
        if (wellFormed) {
            command.text = std::string(*host);
            command.text2 = std::string(*device);
            numbered(2);
        }
    } else if (verb == "audio/channel") {
        // `i first [i mono] [i seq]`: with a request number, mono is said too, so the two are
        // never mistaken for each other.
        command.kind = Kind::Channel;
        const auto first = wholeAt(message, 0);
        wellFormed = first && count >= 1 && count <= 3;
        if (wellFormed) {
            command.number = *first;
            if (count >= 2) {
                const auto mono = wholeAt(message, 1);
                wellFormed = mono && (*mono == 0 || *mono == 1);
                if (wellFormed) {
                    command.flag = *mono == 1;
                }
            }
            if (wellFormed) {
                numbered(2);
            }
        }
    } else if (verb == "source") {
        command.kind = Kind::Source;
        const auto name = textAt(message, 0);
        wellFormed = name && (count == 1 || count == 2);
        if (wellFormed) {
            command.text = std::string(*name);
            numbered(1);
        }
    } else if (verb == "output/add") {
        command.kind = Kind::OutputAdd;
        const auto name = textAt(message, 0);
        const auto kind = textAt(message, 1);
        const auto host = textAt(message, 2);
        const auto port = wholeAt(message, 3);
        wellFormed = name && kind && host && port && (count == 4 || count == 5);
        if (wellFormed) {
            command.text = std::string(*name);
            command.text2 = std::string(*kind);
            command.text3 = std::string(*host);
            command.number = *port;
            numbered(4);
        }
    } else if (verb.rfind("output/", 0) == 0) {
        // `output/<id>/enable|delay|global` — by the row's id, never by its name.
        const std::string_view rest = verb.substr(7);
        const std::size_t slash = rest.find('/');
        const std::string_view id = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        const std::string_view what =
            slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        command.text = std::string(id);
        if (id.empty() || (what != "enable" && what != "delay" && what != "global")) {
            known = false;
        } else if (what == "delay") {
            command.kind = Kind::OutputDelay;
            const std::optional<double> ms = message.number(0);
            wellFormed = ms && std::isfinite(*ms) && (count == 1 || count == 2);
            if (wellFormed) {
                command.value = *ms;
                numbered(1);
            }
        } else {
            command.kind = what == "enable" ? Kind::OutputEnable : Kind::OutputGlobal;
            const auto on = wholeAt(message, 0);
            wellFormed = on && (*on == 0 || *on == 1) && (count == 1 || count == 2);
            if (wellFormed) {
                command.flag = *on == 1;
                numbered(1);
            }
        }
    } else {
        known = false;
    }
    if (!known) {
        return false; // the control surface's own verbs, or nothing at all: `dispatch` says which
    }
    if (!wellFormed) {
        refuse(command, "not understood: see takt4's README for what this address takes");
        return true;
    }
    if (commands_ == nullptr || board_ == nullptr) {
        refuse(command, "takt4 takes no remote changes here");
        return true;
    }
    const RemoteSetup setup = remoteSetup();
    if (!setup.allowed) {
        refuse(command, "remote setup is off: tick \"allow remote setup\" on takt4's OSC row");
        return true;
    }
    // This machine is never refused by the list (see `remote::senderList`).
    const bool loopback = (from.ip >> 24) == 127u;
    if (!setup.from.empty() && !loopback &&
        std::find(setup.from.begin(), setup.from.end(), from.ip) == setup.from.end()) {
        refuse(command, "this sender is not on takt4's list for remote setup");
        return true;
    }
    const auto written = board_->writtenAt();
    if (written == remote::StatusBoard::Clock::time_point{} ||
        std::chrono::duration<double>(remote::StatusBoard::Clock::now() - written).count() >
            kWindowAwaySeconds) {
        refuse(command, "busy: takt4's window is not taking changes now (a dialog may be open)");
        return true;
    }
    if (!commands_->push(command)) {
        refuse(command, "busy: too many changes waiting");
        return true;
    }
    commandsQueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace takt4::control
