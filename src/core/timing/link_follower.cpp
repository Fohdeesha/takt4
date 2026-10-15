#include "core/timing/link_follower.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace takt4::timing {

LinkFollower::LinkFollower(LinkReader& link, LinkListener& listener)
    : LinkFollower(link, listener, Options{}) {}

LinkFollower::LinkFollower(LinkReader& link, LinkListener& listener, Options options)
    : link_(link), listener_(listener), options_(options) {}

std::string LinkFollower::problem() const {
    const std::lock_guard<std::mutex> lock(problemMutex_);
    return problem_;
}

void LinkFollower::publish(std::uint32_t peers, double bpm) noexcept {
    status_.publish(Status{phase_, peers, bpm, canListen_});
}

void LinkFollower::begin(std::uint32_t beatsPerBar) noexcept {
    quantum_ = static_cast<double>(std::max<std::uint32_t>(beatsPerBar, 1));
    // Off until a session has been heard, whatever it was left at: the outputs hand Link over
    // switched off (`output::Transports::setLinkFollowing`), and this makes sure of it.
    link_.enable(false);
    listenAfresh();
}

void LinkFollower::end() noexcept {
    link_.enable(false);
    listener_.close();
    phase_ = Phase::Listening;
    heard_.clear();
    publish(0, 0.0);
}

void LinkFollower::listenAfresh() noexcept {
    phase_ = Phase::Listening;
    heard_.clear();
    listenedAt_ = -1.0;
    // Closed and opened again, which forgets every peer it held: a peer that has gone without a
    // word goes on being listed until its announcement's time to live runs out, and joining on
    // the strength of one is being alone in a session for as long as the join is waited for.
    listener_.close();
    std::string why;
    canListen_ = listener_.open(why);
    {
        const std::lock_guard<std::mutex> lock(problemMutex_);
        problem_ = canListen_ ? std::string() : why;
    }
    publish(0, 0.0);
}

void LinkFollower::listen(double now) {
    if (!canListen_ || (listenedAt_ >= 0.0 && now - listenedAt_ < options_.listenEvery)) {
        return;
    }
    listenedAt_ = now;
    listener_.poll(now);
    heard_ = listener_.heard();
    publish(static_cast<std::uint32_t>(heard_.size()), heard_.empty() ? 0.0 : heard_.front().bpm);
    const auto ready =
        std::find_if(heard_.begin(), heard_.end(), [&](const LinkListener::Heard& peer) {
            return now - peer.firstHeard >= options_.hearFor &&
                   now - peer.lastHeard <= options_.stillHeardWithin;
        });
    if (ready == heard_.end()) {
        return;
    }
    // A session out there for longer than switching on takes to found one: join it, as the
    // youngest session there is. The listening stops — what it has heard is stale by the time it
    // is wanted again, and it is begun afresh then.
    listener_.close();
    link_.enable(true);
    phase_ = Phase::Joining;
    joiningSince_ = now;
    publish(0, ready->bpm);
}

GridReading LinkFollower::read() noexcept {
    const std::chrono::microseconds now = link_.now();
    const double seconds = static_cast<double>(now.count()) / 1e6;
    GridReading reading;
    reading.at = now.count();
    switch (phase_) {
    case Phase::Listening:
        listen(seconds);
        break;
    case Phase::Joining:
        if (link_.numPeers() >= 1) {
            phase_ = Phase::Following;
        } else if (seconds - joiningSince_ >= options_.joinFor) {
            // Heard and not reached. Off again before this lone session has been on long enough
            // to be the older one when somebody does arrive.
            link_.enable(false);
            listenAfresh();
        }
        break;
    case Phase::Following:
        break;
    }
    if (phase_ != Phase::Following) {
        return reading;
    }
    const std::size_t peers = link_.numPeers();
    if (peers == 0) {
        // Everyone else has gone. Off at once: left on and alone, this session would be the older
        // one when the DJ's app came back, and would hand it whatever tempo it was left at.
        link_.enable(false);
        listenAfresh();
        return reading;
    }
    const LinkReader::Timeline timeline = link_.timelineAt(now, quantum_);
    reading.present = timeline.bpm > 0.0;
    reading.bpm = timeline.bpm;
    reading.beat = timeline.beat;
    reading.anchored = true;
    publish(static_cast<std::uint32_t>(peers), timeline.bpm);
    return reading;
}

} // namespace takt4::timing
