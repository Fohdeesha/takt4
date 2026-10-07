#include "core/dmx/artnet_publisher.hpp"

#include <algorithm>
#include <exception>
#include <span>

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
    target.delay = config.delaySeconds;
    target.bit = config.bit;
    target.id = config.id;
    targets_.push_back(std::move(target));
}

bool ArtNetPublisher::setDelay(std::size_t bit, double seconds) noexcept {
    for (Target& target : targets_) {
        if (target.bit == bit) {
            target.delay = seconds;
            return true;
        }
    }
    return false;
}

double ArtNetPublisher::leadSeconds() const noexcept {
    double earliest = 0.0;
    for (const Target& target : targets_) {
        earliest = std::min(earliest, target.delay);
    }
    return -earliest;
}

double ArtNetPublisher::lagOf(std::size_t index) const noexcept {
    return std::max(0.0, targets_[index].delay + leadSeconds());
}

void ArtNetPublisher::forgetHistory() noexcept {
    // Emptied rather than freed: the ring is allocated once, and this runs on the output thread.
    // The next round records the frame as it now is, which is then the oldest there is.
    for (History& history : history_) {
        history.count = 0;
        history.lastRevision = 0;
    }
}

void ArtNetPublisher::record(const DmxEngine& engine, double now) {
    // A universe the patch no longer has is not kept: its history was only ever freed when no
    // node lagged, so re-patching while one did grew it by half a megabyte a universe (the
    // 2026-09-25 audit's L17).
    const std::span<const PortAddress> patched = engine.universes();
    std::erase_if(history_, [patched](const History& history) {
        return std::find(patched.begin(), patched.end(), history.universe) == patched.end();
    });
    for (const PortAddress universe : engine.universes()) {
        const std::span<const std::uint8_t> levels = engine.levels(universe);
        if (levels.size() != kChannelsPerUniverse) {
            continue;
        }
        History* history = nullptr;
        for (History& one : history_) {
            if (one.universe == universe) {
                history = &one;
                break;
            }
        }
        if (history == nullptr) {
            History made;
            made.universe = universe;
            made.ring.resize(static_cast<std::size_t>(kHistorySpan / kHistoryStep) + 8);
            history_.push_back(std::move(made));
            history = &history_.back();
        }
        const std::uint64_t revision = engine.revision(universe);
        if (history->count > 0) {
            const Frame& newest = history->ring[history->newest];
            // Unchanged, or changed again sooner than the history needs: the next round looks
            // again, so a change is never missed, only kept at most every `kHistoryStep` — but a
            // cue started this round is kept this round, so a node sent the lighting late is
            // sent it on its own moment and not a step after, behind the 44 Hz pacing.
            if (revision == history->lastRevision ||
                (!cueThisRound_ && now - newest.at < kHistoryStep)) {
                continue;
            }
        }
        const std::size_t slot =
            history->count == 0 ? 0 : (history->newest + 1) % history->ring.size();
        Frame& frame = history->ring[slot];
        frame.at = now;
        frame.revision = revision;
        std::copy(levels.begin(), levels.end(), frame.levels.begin());
        history->newest = slot;
        history->count = std::min(history->count + 1, history->ring.size());
        history->lastRevision = revision;
    }
}

const ArtNetPublisher::Frame* ArtNetPublisher::frameAt(PortAddress universe,
                                                       double at) const noexcept {
    ++lookups_;
    for (const History& history : history_) {
        if (history.universe != universe || history.count == 0) {
            continue;
        }
        // The frames are in the order they were recorded, so the newest at or before `at` is
        // found by halving — `back` frames behind the newest, the times falling as it grows.
        // It was a walk back from the newest: five hundred frames at a second's delay, for
        // every node and universe, every round (the 2026-09-25 audit's L16).
        const std::size_t size = history.ring.size();
        const auto behind = [&history, size](std::size_t back) -> const Frame& {
            return history.ring[(history.newest + size - back) % size];
        };
        std::size_t low = 0;
        std::size_t high = history.count; // the answer is in [low, high]; count means none
        while (low < high) {
            ++steps_;
            const std::size_t mid = low + (high - low) / 2;
            if (behind(mid).at <= at) {
                high = mid;
            } else {
                low = mid + 1;
            }
        }
        // Before the history reaches that far back, the oldest it has.
        return &behind(std::min(low, history.count - 1));
    }
    return nullptr;
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
        // One still being sent its farewell, back again: it is fed as before rather than
        // fought by the disarmed frames of its own going.
        for (auto going = leaving_.begin(); target.sender == nullptr && going != leaving_.end();
             ++going) {
            if (going->target.id == config.id && going->target.sender->host() == config.host &&
                going->target.sender->port() == config.port) {
                target = std::move(going->target);
                leaving_.erase(going);
                // What it was last sent is its farewell's zeros, whatever the lighting it last
                // had says: sent the lighting again at the next frame, not at the keep-alive —
                // it waited up to 0.9 s dark.
                for (Paced& paced : target.paced) {
                    paced.lastRevision = kNeverSent;
                }
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
        target.delay = config.delaySeconds;
        target.bit = config.bit;
        target.id = config.id;
        next.push_back(std::move(target));
    }
    // Every node not kept has gone, and is sent zeros for a moment: see the header.
    // Not one whose address another node now has — the two would fight over it.
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        if (taken[i] || targets_[i].sender == nullptr) {
            continue;
        }
        const bool reused = std::any_of(next.begin(), next.end(), [&](const Target& kept) {
            return kept.sender->host() == targets_[i].sender->host() &&
                   kept.sender->port() == targets_[i].sender->port();
        });
        if (!reused) {
            leaving_.push_back(Leaving{std::move(targets_[i]), -1.0});
        }
    }
    // **And a farewell still going to an address a new node has just been given** stops: the two
    // would fight over it, the new node's lighting and the old one's zeros in turn.
    std::erase_if(leaving_, [&next](const Leaving& going) {
        return std::any_of(next.begin(), next.end(), [&going](const Target& kept) {
            return kept.sender->host() == going.target.sender->host() &&
                   kept.sender->port() == going.target.sender->port();
        });
    });
    targets_ = std::move(next);
    return failures;
}

bool ArtNetPublisher::sendFarewell(const DmxEngine& engine, Target& target, PortAddress universe,
                                   double now) {
    if (engine.levels(universe).size() != kChannelsPerUniverse) {
        return false;
    }
    static constexpr std::array<std::uint8_t, kChannelsPerUniverse> kDark{};
    Paced& paced = pacedFor(target, universe);
    if (target.sender->sendDmx(universe, kDark)) {
        ++sent_;
    } else {
        ++failed_;
    }
    paced.lastSentAt = now;
    paced.everSent = true;
    return true;
}

void ArtNetPublisher::refresh() noexcept {
    for (Target& target : targets_) {
        target.sender->refresh();
    }
    for (Leaving& going : leaving_) {
        going.target.sender->refresh();
    }
}

const ArtNetPublisher::Paced* ArtNetPublisher::findPaced(const Target& target,
                                                         PortAddress universe) noexcept {
    for (const Paced& paced : target.paced) {
        if (paced.universe == universe) {
            return &paced;
        }
    }
    return nullptr;
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

void ArtNetPublisher::setUpcomingCues(std::span<const std::pair<PortAddress, double>> cues) {
    upcoming_.assign(cues.begin(), cues.end());
}

void ArtNetPublisher::cueStarted(PortAddress universe, double at) {
    // Only a node sent the lighting late sees a cue after it has started; with every node on time
    // there is nothing to keep, and a cue reaching many fixtures in a universe is kept once.
    if (!lagging_) {
        return;
    }
    const bool known = std::any_of(started_.begin(), started_.end(), [&](const auto& cue) {
        return cue.first == universe && cue.second == at;
    });
    if (!known) {
        started_.emplace_back(universe, at);
    }
    cueThisRound_ = true;
}

bool ArtNetPublisher::cueSoon(PortAddress universe, double lag, double now) const noexcept {
    const auto soon = [lag, now](double at) {
        const double seen = at + lag;
        return seen > now && seen <= now + kMinFramePeriod;
    };
    for (const auto& [cued, at] : upcoming_) {
        if (cued == universe && soon(at)) {
            return true;
        }
    }
    for (const auto& [cued, at] : started_) {
        if (cued == universe && soon(at)) {
            return true;
        }
    }
    return false;
}

std::size_t ArtNetPublisher::publish(const DmxEngine& engine, double now) {
    std::size_t datagrams = 0;
    const double step = lastPublish_ < 0.0 ? 0.0 : std::max(0.0, now - lastPublish_);
    lastPublish_ = now;
    std::erase_if(started_, [now](const auto& cue) { return now - cue.second > kHistorySpan; });
    // **A node's lag grows no faster than time passes** — what it is sent stands still while it
    // does — and shrinks at once. Taken at once both ways, a delay dragged later sent a node the
    // last stretch of lighting a second time, so a flash it had shown was shown again; one dragged
    // earlier skips the stretch between, which cannot be helped.
    //
    // And the history, only while a node is sent the lighting later than it is made; with every
    // node on time it would be half a megabyte a universe for nothing.
    bool lagging = false;
    for (std::size_t i = 0; i < targets_.size(); ++i) {
        Target& target = targets_[i];
        const double wanted = lagOf(i);
        target.lag = target.lag < 0.0 || wanted < target.lag ? wanted
                                                              : std::min(wanted, target.lag + step);
        lagging = lagging || target.lag > 0.0;
    }
    lagging_ = lagging;
    if (lagging) {
        record(engine, now);
    } else {
        history_.clear();
        started_.clear();
    }
    cueThisRound_ = false;
    for (Target& target : targets_) {
        const double lag = target.lag;
        // The patch's universes, and then those it has just dropped, which are sent their zeros
        // like any other frame (the audit of 2026-09-25, H4 — see `DmxEngine::released`).
        const std::size_t patched = engine.universes().size();
        const std::size_t total = patched + engine.released().size();
        for (std::size_t u = 0; u < total; ++u) {
            const bool released = u >= patched;
            const PortAddress universe =
                released ? engine.released()[u - patched] : engine.universes()[u];
            // Nothing is due within the 44 Hz period after the last frame sent, whatever has
            // moved, so such a round does not look the past frame up at all — the other half
            // of the audit of 2026-09-25's L16, beside the halving in `frameAt`.
            if (const Paced* const known = findPaced(target, universe);
                known != nullptr && known->everSent && now - known->lastSentAt < kMinFramePeriod) {
                continue;
            }
            // Nothing in the period before a cue this node will see — see the header.
            if (!released && cueSoon(universe, lag, now)) {
                continue;
            }
            std::span<const std::uint8_t> levels = engine.levels(universe);
            std::uint64_t revision = engine.revision(universe);
            // Not a released one: its history still holds the lit frames from before it was
            // dropped, and "dark, now" is the whole point of releasing it.
            if (lag > 0.0 && !released) {
                // What this universe was `lag` ago. Before the history reaches that far back —
                // a delay just set — the oldest it has, which is the nearest thing to it.
                // A nanosecond's grace: the frame kept at the very instant asked for is that
                // instant's, whichever way the subtraction rounded.
                if (const Frame* const past = frameAt(universe, now - lag + 1e-9)) {
                    levels = std::span<const std::uint8_t>(past->levels);
                    revision = past->revision;
                }
            }
            if (levels.empty()) {
                continue;
            }

            Paced& paced = pacedFor(target, universe);
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
            // **A frame that did not go is not the frame sent**: the 44 Hz pacing counts from the
            // attempt, and the lighting it carried is tried again at the next frame. It used to be
            // recorded as sent, so a static look a failing node missed waited for the 0.9 s
            // keep-alive.
            const bool went = target.sender->sendDmx(universe, levels);
            if (went) {
                ++sent_;
                ++datagrams;
                paced.lastRevision = revision;
            } else {
                ++failed_;
            }
            paced.lastSentAt = now;
            paced.everSent = true;
        }
    }
    // The nodes that have gone: all zeros, at the 44 Hz pace whatever has moved, for a released
    // universe's time — and at once, whatever their delay: this is the way out. See `setTargets`.
    for (Leaving& going : leaving_) {
        if (going.until < 0.0) {
            going.until = now + kFarewellSeconds;
        }
        const std::size_t patched = engine.universes().size();
        const std::size_t total = patched + engine.released().size();
        for (std::size_t u = 0; u < total; ++u) {
            const PortAddress universe =
                u >= patched ? engine.released()[u - patched] : engine.universes()[u];
            if (const Paced* const known = findPaced(going.target, universe);
                known != nullptr && known->everSent && now - known->lastSentAt < kMinFramePeriod) {
                continue;
            }
            if (sendFarewell(engine, going.target, universe, now)) {
                ++datagrams;
            }
        }
    }
    std::erase_if(leaving_, [now](const Leaving& going) { return now >= going.until; });
    return datagrams;
}

double ArtNetPublisher::secondsUntilPaced(double now) const noexcept {
    double wait = 0.0;
    const auto pace = [&wait, now](const Target& target) {
        for (const Paced& paced : target.paced) {
            if (paced.everSent) {
                wait = std::max(wait, paced.lastSentAt + kMinFramePeriod - now);
            }
        }
    };
    for (const Target& target : targets_) {
        pace(target);
    }
    for (const Leaving& going : leaving_) {
        pace(going.target);
    }
    return std::clamp(wait, 0.0, kMinFramePeriod);
}

std::size_t ArtNetPublisher::flush(const DmxEngine& engine, double now) {
    std::size_t datagrams = 0;
    // The frame as it is now, to every node whatever its delay: this is the last thing sent,
    // and a delayed node would otherwise never be sent it at all. A universe just released
    // included: its zeros are the last word too.
    const std::size_t patched = engine.universes().size();
    const std::size_t total = patched + engine.released().size();
    for (Target& target : targets_) {
        for (std::size_t u = 0; u < total; ++u) {
            const PortAddress universe =
                u >= patched ? engine.released()[u - patched] : engine.universes()[u];
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
    // And a node still being sent its farewell, its last word dark.
    for (Leaving& going : leaving_) {
        for (std::size_t u = 0; u < total; ++u) {
            const PortAddress universe =
                u >= patched ? engine.released()[u - patched] : engine.universes()[u];
            if (sendFarewell(engine, going.target, universe, now)) {
                ++datagrams;
            }
        }
    }
    return datagrams;
}

} // namespace takt4::dmx
