#include "core/dmx/artnet_publisher.hpp"

#include <algorithm>
#include <exception>

namespace takt4::dmx {
namespace {

/// The shortest gap between two frames of one universe to one node — the 44 Hz ceiling, as a
/// period. Computed once rather than written as 0.0227, so that the number and the constant
/// it comes from cannot drift apart.
constexpr double kMinFramePeriod = 1.0 / kMaxRefreshHz;

} // namespace

void ArtNetPublisher::addTarget(const TargetConfig& config) {
    Target target;
    target.sender = std::make_unique<ArtNetSender>(config.host, config.port);
    target.universes = config.universes;
    target.bit = config.bit;
    target.id = config.id;
    std::sort(target.universes.begin(), target.universes.end());
    target.universes.erase(std::unique(target.universes.begin(), target.universes.end()),
                           target.universes.end());
    targets_.push_back(std::move(target));
}

std::vector<std::pair<std::size_t, std::string>>
ArtNetPublisher::setTargets(const std::vector<TargetConfig>& configs) {
    std::vector<std::pair<std::size_t, std::string>> failures;
    std::vector<Target> next;
    next.reserve(configs.size());
    std::vector<bool> taken(targets_.size(), false);
    for (const TargetConfig& config : configs) {
        Target target;
        for (std::size_t i = 0; i < targets_.size() && !config.id.empty(); ++i) {
            if (!taken[i] && targets_[i].id == config.id &&
                targets_[i].sender->host() == config.host &&
                targets_[i].sender->port() == config.port) {
                // The same node at the same address: its sender, its sequence numbers and its
                // pacing clocks carry on, so it goes on being fed at the rate it was.
                taken[i] = true;
                target = std::move(targets_[i]);
                break;
            }
        }
        if (target.sender == nullptr) {
            try {
                target.sender = std::make_unique<ArtNetSender>(config.host, config.port);
            } catch (const std::exception& e) {
                failures.emplace_back(config.bit, e.what());
                continue;
            }
        }
        target.universes = config.universes;
        std::sort(target.universes.begin(), target.universes.end());
        target.universes.erase(std::unique(target.universes.begin(), target.universes.end()),
                               target.universes.end());
        target.bit = config.bit;
        target.id = config.id;
        next.push_back(std::move(target));
    }
    targets_ = std::move(next);
    return failures;
}

void ArtNetPublisher::refresh() noexcept {
    for (Target& target : targets_) {
        (void)target.sender->ready();
    }
}

void ArtNetPublisher::clearTargets() noexcept {
    targets_.clear();
}

ArtNetPublisher::Paced& ArtNetPublisher::pacedFor(Target& target, PortAddress universe) {
    for (Paced& paced : target.paced) {
        if (paced.universe == universe) {
            return paced;
        }
    }
    target.paced.push_back(Paced{universe, -1.0, 0, false});
    return target.paced.back();
}

std::size_t ArtNetPublisher::publish(const DmxEngine& engine, double now) {
    std::size_t datagrams = 0;
    for (Target& target : targets_) {
        // An empty universe list means every universe the patch uses, which is what one node
        // on one rig means and what the field can be left alone for.
        const std::vector<PortAddress>& carried =
            target.universes.empty() ? engine.universes() : target.universes;

        for (const PortAddress universe : carried) {
            const std::span<const std::uint8_t> levels = engine.levels(universe);
            if (levels.empty()) {
                // A target configured for a universe the patch does not use. Not an error: an
                // operator who has unpatched a fixture for tonight should not have to edit the
                // node's universe list as well.
                continue;
            }

            Paced& paced = pacedFor(target, universe);
            const std::uint64_t revision = engine.revision(universe);
            const double since = now - paced.lastSentAt;

            bool due = false;
            if (!paced.everSent) {
                // The first frame goes out at once. A node that has just been pointed at
                // should light up now, not within a keep-alive.
                due = true;
            } else if (revision != paced.lastRevision) {
                // Something moved. Send it as soon as the 44 Hz ceiling allows — and no
                // sooner, because a thousand frames a second is how a fade becomes a stutter.
                due = since >= kMinFramePeriod;
            } else {
                due = since >= kKeepAliveSeconds;
            }
            if (!due) {
                continue;
            }

            // Always the full 512 channels. The specification allows any even length from 2
            // and explicitly blesses the other choice — *"products which convert Art-Net to
            // DMX512 may opt to always send 512 channels"* — and a fixed length is what every
            // node has certainly been tested against. At 44 Hz it is 23 KB/s per universe,
            // which is not a number worth optimising on a wired rig and not a protocol worth
            // running on a wireless one.
            if (target.sender->sendDmx(universe, levels)) {
                ++sent_;
                ++datagrams;
            } else {
                ++failed_;
            }
            paced.lastSentAt = now;
            paced.lastRevision = revision;
            paced.everSent = true;
        }
    }
    return datagrams;
}

std::size_t ArtNetPublisher::flush(const DmxEngine& engine, double now) {
    std::size_t datagrams = 0;
    for (Target& target : targets_) {
        const std::vector<PortAddress>& carried =
            target.universes.empty() ? engine.universes() : target.universes;
        for (const PortAddress universe : carried) {
            const std::span<const std::uint8_t> levels = engine.levels(universe);
            if (levels.empty()) {
                continue;
            }
            Paced& paced = pacedFor(target, universe);
            if (target.sender->sendDmx(universe, levels)) {
                ++sent_;
                ++datagrams;
            } else {
                ++failed_;
            }
            paced.lastSentAt = now;
            paced.lastRevision = engine.revision(universe);
            paced.everSent = true;
        }
    }
    return datagrams;
}

} // namespace takt4::dmx
