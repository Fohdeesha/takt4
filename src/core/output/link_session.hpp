#pragma once

#include <cstddef>
#include <memory>

namespace takt4::output {

/// Owns an Ableton Link instance.
///
/// Construction starts Link's service thread but does not touch the network; nothing is
/// visible to peers until the session is enabled. The accessors here take app-thread
/// snapshots and are not for use from the audio callback.
class LinkSession {
public:
    explicit LinkSession(double initialTempoBpm);
    ~LinkSession();

    LinkSession(const LinkSession&) = delete;
    LinkSession& operator=(const LinkSession&) = delete;

    bool enabled() const;
    std::size_t numPeers() const;
    double tempoBpm() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace takt4::output
