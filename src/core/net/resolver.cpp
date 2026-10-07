#include "core/net/resolver.hpp"

#include "core/net/udp.hpp"
#include "core/sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string_view>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace takt4::net {
namespace {

using Clock = std::chrono::steady_clock;

static_assert(sizeof(sockaddr_storage) <= sizeof(AsyncAddress::Address::storage),
              "AsyncAddress::Address has to hold any address the platform can hand back");

/// The best of what a look-up returned: the first IPv4 address, or else the first IPv6 one.
std::optional<AsyncAddress::Address> pick(const addrinfo* results) {
    const addrinfo* chosen = nullptr;
    for (const addrinfo* candidate = results; candidate != nullptr;
         candidate = candidate->ai_next) {
        if (candidate->ai_family == AF_INET) {
            chosen = candidate;
            break;
        }
        if (chosen == nullptr && candidate->ai_family == AF_INET6) {
            chosen = candidate;
        }
    }
    if (chosen == nullptr) {
        return std::nullopt;
    }
    AsyncAddress::Address out;
    std::memcpy(out.storage, chosen->ai_addr, chosen->ai_addrlen);
    out.length = static_cast<int>(chosen->ai_addrlen);
    out.family = chosen->ai_family;
    return out;
}

/// Whether this is the one name the test sandbox lets through: `localhost`, which the system
/// answers itself and asks nobody about.
bool isLocalhost(const std::string& host) noexcept {
    constexpr std::string_view name = "localhost";
    return host.size() == name.size() &&
           std::equal(host.begin(), host.end(), name.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == b;
           });
}

/// `AsyncAddress::answerForTests`'s answers.
std::mutex& testAnswersMutex() {
    static std::mutex mutex;
    return mutex;
}
std::vector<std::pair<std::string, std::string>>& testAnswers() {
    static std::vector<std::pair<std::string, std::string>> answers;
    return answers;
}

std::optional<std::string> testAnswer(const std::string& host) {
    const std::lock_guard<std::mutex> lock(testAnswersMutex());
    for (const auto& [name, numeric] : testAnswers()) {
        if (name == host) {
            return numeric;
        }
    }
    return std::nullopt;
}

/// One look-up. `numericOnly` asks nobody — `AI_NUMERICHOST` fails at once on a name — which is
/// what makes the constructor safe to call from the output thread. Zero, or the error.
int resolve(const std::string& host, std::uint16_t port, bool numericOnly,
            std::optional<AsyncAddress::Address>& out) {
    // In the test binaries, a name goes nowhere: see `sandbox.hpp`. -2 is not one of
    // `getaddrinfo`'s codes, so a status line that shows it is recognisably this.
    if (!numericOnly && sandbox::active()) {
        if (const std::optional<std::string> answer = testAnswer(host)) {
            return resolve(*answer, port, true, out);
        }
        if (!isLocalhost(host)) {
            sandbox::refuse(sandbox::Refused::Lookup);
            return -2;
        }
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = numericOnly ? AI_NUMERICHOST : 0;
    addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int error = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &results);
    if (error != 0 || results == nullptr) {
        return error != 0 ? error : -1;
    }
    out = pick(results);
    ::freeaddrinfo(results);
    return out ? 0 : -1;
}

} // namespace

struct AsyncAddress::State {
    /// `getaddrinfo` needs Winsock up on the look-up thread as much as anywhere.
    WinsockGuard winsock;
    mutable std::mutex mutex;
    std::optional<Address> address;
    bool looking = false;
    int error = 0;
    Clock::time_point failedAt{};
    /// When the name was last asked about, whatever the answer: a refresh is due
    /// `refreshSeconds_` after it.
    Clock::time_point askedAt{};
    std::uint64_t lookUps = 0;
};

AsyncAddress::AsyncAddress(std::string host, std::uint16_t port, double refreshSeconds)
    : host_(std::move(host)), port_(port), refreshSeconds_(refreshSeconds),
      state_(std::make_shared<State>()) {
    std::optional<Address> numeric;
    if (resolve(host_, port_, /*numericOnly=*/true, numeric) == 0) {
        state_->address = numeric;
        return;
    }
    lookUp();
}

AsyncAddress::~AsyncAddress() = default;

void AsyncAddress::lookUp() {
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->looking = true;
        state_->error = 0;
        ++state_->lookUps;
    }
    try {
        std::thread([state = state_, host = host_, port = port_] {
            std::optional<Address> found;
            const int error = resolve(host, port, /*numericOnly=*/false, found);
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->looking = false;
            state->askedAt = Clock::now();
            if (error == 0) {
                state->address = found;
            } else if (!state->address) {
                // A refresh that fails keeps the address it had: the name is still the best
                // guess there is, and a gap in the sending would be worse than an old address.
                state->error = error;
                state->failedAt = state->askedAt;
            }
        }).detach();
    } catch (const std::exception&) {
        // No thread to be had. Counted as a failed look-up, so it is tried again later rather
        // than never, and said rather than hidden.
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->looking = false;
        state_->error = -1;
        state_->failedAt = Clock::now();
    }
}

void AsyncAddress::answerForTests(const std::string& host, const std::string& numeric) {
    const std::lock_guard<std::mutex> lock(testAnswersMutex());
    for (auto& [name, answer] : testAnswers()) {
        if (name == host) {
            answer = numeric;
            return;
        }
    }
    testAnswers().emplace_back(host, numeric);
}

std::optional<AsyncAddress::Address> AsyncAddress::address() {
    bool retry = false;
    std::optional<Address> known;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        const Clock::time_point now = Clock::now();
        if (state_->address) {
            known = state_->address;
            // A name, asked again once its answer is `refreshSeconds_` old; a number never was
            // asked (`lookUps` is 0) and is never asked.
            retry = state_->lookUps > 0 && !state_->looking &&
                    std::chrono::duration<double>(now - state_->askedAt).count() >=
                        refreshSeconds_;
        } else {
            retry = !state_->looking && state_->error != 0 &&
                    std::chrono::duration<double>(now - state_->failedAt).count() >=
                        kRetrySeconds;
        }
    }
    if (retry) {
        lookUp();
    }
    return known;
}

std::uint64_t AsyncAddress::lookUps() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->lookUps;
}

std::string AsyncAddress::problem() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->address) {
        return {};
    }
    if (state_->looking) {
        return "looking up " + host_;
    }
    // The code rather than `gai_strerror`'s words: on Windows that returns a static buffer,
    // which two look-up threads failing together would write over each other.
    return "cannot resolve " + host_ + " (" + std::to_string(state_->error) +
           "); trying again every few seconds";
}

} // namespace takt4::net
