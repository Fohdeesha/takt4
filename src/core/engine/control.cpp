#include "core/engine/control.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace takt4::engine {

ControlQueue::ControlQueue() {
    pending_.reserve(kCapacity);
}

bool ControlQueue::post(const Command& command) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (command.kind == Command::Kind::SetTempoOptions) {
        // Settings are state, not an action: the newest says everything an older one
        // did, and the older one was never seen by anything. Dropping it — rather than
        // the newcomer taking its slot — is what keeps the rest in the order they were
        // posted, so a ÷2 pressed between two slider moves still lands between them.
        const auto stale = std::remove_if(pending_.begin(), pending_.end(), [](const Command& c) {
            return c.kind == Command::Kind::SetTempoOptions;
        });
        pending_.erase(stale, pending_.end());
    }
    if (pending_.size() >= kCapacity && isSetting(command.kind)) {
        const auto press = std::find_if(pending_.begin(), pending_.end(),
                                        [](const Command& c) { return !isSetting(c.kind); });
        if (press != pending_.end()) {
            pending_.erase(press);
            ++dropped_;
        }
    }
    if (pending_.size() >= kCapacity) {
        ++dropped_;
        return false;
    }
    pending_.push_back(command);
    return true;
}

void ControlQueue::dropPresses() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::erase_if(pending_, [](const Command& c) { return !isSetting(c.kind); });
}

void ControlQueue::drain(std::vector<Command>& out) {
    out.clear();
    const std::lock_guard<std::mutex> lock(mutex_);
    out.swap(pending_);
    // `pending_` now holds whatever buffer the caller brought, which on the first call is
    // an empty one. Sizing it here, on the consumer, is what keeps `post` from ever
    // allocating on a UI or a network thread.
    pending_.reserve(kCapacity);
}

std::size_t ControlQueue::pending() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

std::uint64_t ControlQueue::dropped() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

} // namespace takt4::engine
