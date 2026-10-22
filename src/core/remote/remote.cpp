#include "core/remote/remote.hpp"

#include "core/settings/settings.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace takt4::remote {

std::string Endpoint::text() const {
    return std::to_string((ip >> 24) & 0xFFu) + "." + std::to_string((ip >> 16) & 0xFFu) + "." +
           std::to_string((ip >> 8) & 0xFFu) + "." + std::to_string(ip & 0xFFu) + ":" +
           std::to_string(port);
}

void StatusBoard::publish(Status status, Clock::time_point at) {
    const std::lock_guard<std::mutex> lock(mutex_);
    status_ = std::move(status);
    at_ = at;
}

Status StatusBoard::read(Clock::time_point* at) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (at != nullptr) {
        *at = at_;
    }
    return status_;
}

bool StatusBoard::tryRead(Status& out, Clock::time_point& at) const {
    const std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    out = status_;
    at = at_;
    return true;
}

StatusBoard::Clock::time_point StatusBoard::writtenAt() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return at_;
}

bool CommandQueue::push(Command command) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kCapacity) {
        return false;
    }
    command.queuedAt = std::chrono::steady_clock::now();
    queue_.push_back(std::move(command));
    return true;
}

std::vector<Command> CommandQueue::drain() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Command> out(std::make_move_iterator(queue_.begin()),
                             std::make_move_iterator(queue_.end()));
    queue_.clear();
    return out;
}

void LogQueue::push(LogLine line) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kCapacity) {
        ++dropped_;
        return;
    }
    queue_.push_back(std::move(line));
}

std::vector<LogLine> LogQueue::take(std::size_t most) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t count = std::min(most, queue_.size());
    const auto end = queue_.begin() + static_cast<std::ptrdiff_t>(count);
    std::vector<LogLine> out(std::make_move_iterator(queue_.begin()), std::make_move_iterator(end));
    queue_.erase(queue_.begin(), end);
    return out;
}

std::uint64_t LogQueue::dropped() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

namespace {

/// The window's picker order: beat detection, Link, MIDI clock, the CDJs.
constexpr std::array<std::string_view, 4> kSources{settings::kTimingAudio, settings::kTimingLink,
                                                   settings::kTimingMidiClock,
                                                   settings::kTimingProLink};

} // namespace

std::string_view sourceName(int timingIndex) noexcept {
    if (timingIndex < 0 || timingIndex >= static_cast<int>(kSources.size())) {
        return settings::kTimingAudio;
    }
    return kSources[static_cast<std::size_t>(timingIndex)];
}

std::optional<int> sourceIndex(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kSources.size(); ++i) {
        if (kSources[i] == name) {
            return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

std::vector<std::uint32_t> senderList(std::string_view text, std::vector<std::string>* rejected) {
    std::vector<std::uint32_t> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find_first_of(", ;\t\r\n", at);
        const std::string_view word =
            text.substr(at, end == std::string_view::npos ? text.size() - at : end - at);
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (word.empty()) {
            continue;
        }
        // Four numbers 0 to 255, dotted, and nothing else: no names (a name would be looked up,
        // and a lookup is a question somebody else answers), no ports, no masks.
        std::uint32_t address = 0;
        int parts = 0;
        bool good = true;
        std::size_t p = 0;
        while (good && p <= word.size() && parts < 4) {
            std::size_t q = p;
            std::uint32_t value = 0;
            while (q < word.size() && word[q] >= '0' && word[q] <= '9' && q - p < 3) {
                value = value * 10 + static_cast<std::uint32_t>(word[q] - '0');
                ++q;
            }
            good = q > p && value <= 255;
            address = (address << 8) | value;
            ++parts;
            if (parts < 4) {
                good = good && q < word.size() && word[q] == '.';
                p = q + 1;
            } else {
                good = good && q == word.size();
            }
        }
        if (good && parts == 4) {
            if (std::find(out.begin(), out.end(), address) == out.end()) {
                out.push_back(address);
            }
        } else if (rejected != nullptr) {
            rejected->emplace_back(word);
        }
    }
    return out;
}

} // namespace takt4::remote
