#include "core/net/resolver.hpp"

#include "core/net/udp.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

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

/// One look-up. `numericOnly` asks nobody — `AI_NUMERICHOST` fails at once on a name — which is
/// what makes the constructor safe to call from the output thread. Zero, or the error.
int resolve(const std::string& host, std::uint16_t port, bool numericOnly,
            std::optional<AsyncAddress::Address>& out) {
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
};

AsyncAddress::AsyncAddress(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port), state_(std::make_shared<State>()) {
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
    }
    try {
        std::thread([state = state_, host = host_, port = port_] {
            std::optional<Address> found;
            const int error = resolve(host, port, /*numericOnly=*/false, found);
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->looking = false;
            if (error == 0) {
                state->address = found;
            } else {
                state->error = error;
                state->failedAt = Clock::now();
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

std::optional<AsyncAddress::Address> AsyncAddress::address() {
    bool retry = false;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->address) {
            return state_->address;
        }
        retry = !state_->looking && state_->error != 0 &&
                std::chrono::duration<double>(Clock::now() - state_->failedAt).count() >=
                    kRetrySeconds;
    }
    if (retry) {
        lookUp();
    }
    return std::nullopt;
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
