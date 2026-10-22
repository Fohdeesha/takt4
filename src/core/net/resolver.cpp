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

using Address = AsyncAddress::Address;

/// Every IPv4 and IPv6 address a look-up returned, in its order.
std::vector<Address> candidatesOf(const addrinfo* results) {
    std::vector<Address> all;
    for (const addrinfo* result = results; result != nullptr; result = result->ai_next) {
        if (result->ai_family != AF_INET && result->ai_family != AF_INET6) {
            continue;
        }
        Address address;
        std::memcpy(address.storage, result->ai_addr, result->ai_addrlen);
        address.length = static_cast<int>(result->ai_addrlen);
        address.family = result->ai_family;
        all.push_back(address);
    }
    return all;
}

bool same(const Address& a, const Address& b) {
    return a.family == b.family && a.length == b.length &&
           std::memcmp(a.storage, b.storage, static_cast<std::size_t>(a.length)) == 0;
}

/// The best of what a look-up returned: `held` while the answer still has it — a name with
/// several addresses answers them in any order, and taking the first moved a node from one to
/// another every refresh — and otherwise the first IPv4 address, or else the first IPv6 one.
std::optional<Address> pick(const std::vector<Address>& candidates,
                            const std::optional<Address>& held) {
    if (held && std::any_of(candidates.begin(), candidates.end(),
                            [&held](const Address& candidate) { return same(candidate, *held); })) {
        return held;
    }
    const Address* chosen = nullptr;
    for (const Address& candidate : candidates) {
        if (candidate.family == AF_INET) {
            chosen = &candidate;
            break;
        }
        if (chosen == nullptr && candidate.family == AF_INET6) {
            chosen = &candidate;
        }
    }
    return chosen != nullptr ? std::optional<Address>(*chosen) : std::nullopt;
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
struct TestAnswer {
    std::string host;
    std::string numeric;
    double delaySeconds = 0.0;
};
std::mutex& testAnswersMutex() {
    static std::mutex mutex;
    return mutex;
}
std::vector<TestAnswer>& testAnswers() {
    static std::vector<TestAnswer> answers;
    return answers;
}

std::optional<TestAnswer> testAnswer(const std::string& host) {
    const std::lock_guard<std::mutex> lock(testAnswersMutex());
    for (const TestAnswer& answer : testAnswers()) {
        if (answer.host == host) {
            return answer;
        }
    }
    return std::nullopt;
}

/// Every address of one look-up. `numericOnly` asks nobody — `AI_NUMERICHOST` fails at once on
/// a name — which is what makes the constructor safe to call from the output thread. Zero, or
/// the error.
int lookUpAll(const std::string& host, std::uint16_t port, bool numericOnly,
              std::vector<Address>& out) {
    // In the test binaries, a name goes nowhere: see `sandbox.hpp`. -2 is not one of
    // `getaddrinfo`'s codes, so a status line that shows it is recognisably this.
    if (!numericOnly && sandbox::active()) {
        if (const std::optional<TestAnswer> answer = testAnswer(host)) {
            if (answer->delaySeconds > 0.0) {
                std::this_thread::sleep_for(std::chrono::duration<double>(answer->delaySeconds));
            }
            // Comma-separated, a name with several addresses, in the order a test gives them.
            std::size_t from = 0;
            for (;;) {
                const std::size_t comma = answer->numeric.find(',', from);
                std::vector<Address> one;
                const int error =
                    lookUpAll(answer->numeric.substr(from, comma - from), port, true, one);
                if (error != 0) {
                    return error;
                }
                out.insert(out.end(), one.begin(), one.end());
                if (comma == std::string::npos) {
                    break;
                }
                from = comma + 1;
            }
            return out.empty() ? -1 : 0;
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
    out = candidatesOf(results);
    ::freeaddrinfo(results);
    return out.empty() ? -1 : 0;
}

/// One look-up's answer: `held`, the address the name already has, while the answer still has it
/// (`pick`). Zero, or the error.
int resolve(const std::string& host, std::uint16_t port, bool numericOnly,
            std::optional<Address>& out, const std::optional<Address>& held = std::nullopt) {
    std::vector<Address> candidates;
    if (const int error = lookUpAll(host, port, numericOnly, candidates); error != 0) {
        return error;
    }
    out = pick(candidates, held);
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
    /// When the look-up going now was started, for `problem` to say when it has gone on too long.
    Clock::time_point lookingSince{};
    std::uint64_t lookUps = 0;
};

AsyncAddress::AsyncAddress(std::string host, std::uint16_t port, double refreshSeconds,
                           double slowSeconds)
    : host_(std::move(host)), port_(port), refreshSeconds_(refreshSeconds),
      slowSeconds_(slowSeconds), state_(std::make_shared<State>()) {
    std::optional<Address> numeric;
    if (resolve(host_, port_, /*numericOnly=*/true, numeric) == 0) {
        state_->address = numeric;
        return;
    }
    lookUp();
}

AsyncAddress::~AsyncAddress() = default;

void AsyncAddress::lookUp() {
    std::optional<Address> held;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->looking = true;
        state_->lookingSince = Clock::now();
        state_->error = 0;
        ++state_->lookUps;
        held = state_->address;
    }
    try {
        std::thread([state = state_, host = host_, port = port_, held] {
            std::optional<Address> found;
            const int error = resolve(host, port, /*numericOnly=*/false, found, held);
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

void AsyncAddress::answerForTests(const std::string& host, const std::string& numeric,
                                  double delaySeconds) {
    const std::lock_guard<std::mutex> lock(testAnswersMutex());
    for (TestAnswer& answer : testAnswers()) {
        if (answer.host == host) {
            answer.numeric = numeric;
            answer.delaySeconds = delaySeconds;
            return;
        }
    }
    testAnswers().push_back(TestAnswer{host, numeric, delaySeconds});
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
        const double waited =
            std::chrono::duration<double>(Clock::now() - state_->lookingSince).count();
        if (waited < slowSeconds_) {
            return "looking up " + host_;
        }
        // Not "looking up": a status line passes that over as the moment every named output
        // has at launch, and a name server that never answers made it the output's word for good.
        return "no answer about " + host_ + " after " +
               std::to_string(static_cast<long long>(waited)) +
               " s \xE2\x80\x94 is the name right, and a name server reachable?";
    }
    // The code rather than `gai_strerror`'s words: on Windows that returns a static buffer,
    // which two look-up threads failing together would write over each other.
    return "cannot resolve " + host_ + " (" + std::to_string(state_->error) +
           "); trying again every few seconds";
}

} // namespace takt4::net
