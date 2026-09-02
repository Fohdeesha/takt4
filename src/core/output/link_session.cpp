#include "core/output/link_session.hpp"

#include <ableton/Link.hpp>

namespace takt4::output {

struct LinkSession::Impl {
    explicit Impl(double bpm) : link(bpm) {}
    ableton::Link link;
};

LinkSession::LinkSession(double initialTempoBpm) : impl_(std::make_unique<Impl>(initialTempoBpm)) {}

LinkSession::~LinkSession() = default;

bool LinkSession::enabled() const {
    return impl_->link.isEnabled();
}

std::size_t LinkSession::numPeers() const {
    return impl_->link.numPeers();
}

double LinkSession::tempoBpm() const {
    return impl_->link.captureAppSessionState().tempo();
}

} // namespace takt4::output
