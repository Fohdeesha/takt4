#pragma once

// What every click sweep ends by checking: that no click on the way opened anything real — a
// socket bound to a port, a MIDI port, a Link session, an audio device (the audit of 2026-09-25,
// T2). A sweep presses whatever the layout has put under it, and one that crossed the OSC
// "listen" box bound the control port on the machine the show runs from, thirteen times, while
// its test passed. The sandbox refuses all of these now (src/core/sandbox.hpp); a refusal during
// a sweep is a sweep that has wandered off what it was looking for, and this makes that a failure
// rather than a line on stderr.
//
// REQUIRE-free, and CHECKs only, so it can be called at the end of a test.

#include "core/sandbox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace takt4::tests {

class NothingReal {
public:
    NothingReal() {
        for (std::size_t i = 0; i < kKinds.size(); ++i) {
            before_[i] = sandbox::refusals(kKinds[i]);
        }
    }

    void check() const {
        for (std::size_t i = 0; i < kKinds.size(); ++i) {
            INFO("refusals of kind " << static_cast<int>(kKinds[i])
                                     << " (2 bind, 3 MIDI, 4 Link, 5 audio)");
            CHECK(sandbox::refusals(kKinds[i]) == before_[i]);
        }
    }

private:
    static constexpr std::array<sandbox::Refused, 4> kKinds = {
        sandbox::Refused::Bind, sandbox::Refused::Midi, sandbox::Refused::Link,
        sandbox::Refused::Audio};
    std::array<std::uint64_t, kKinds.size()> before_{};
};

} // namespace takt4::tests
