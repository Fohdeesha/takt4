// The sandbox's switch, test by test (src/core/sandbox.hpp).
//
// On for every test. Off for the length of one tagged [network] or [hardware]: those are the
// tests the `-all` presets exist to run, and the only ones allowed to reach the rig — which is
// what the tags have meant since the audit's T2, and is now enforced rather than hoped for. An
// untagged test that needs a real device, a real session or a real socket fails, which is the
// point: untagged is a promise that a test stays off the machine the show runs from, in the
// `-all` run as much as in the default one.
//
// The environment follows, so a process such a test starts is sandboxed exactly as it is.

#include "core/sandbox.hpp"

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

class RigSandbox final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(const Catch::TestCaseInfo& info) override {
        bool rig = false;
        for (const Catch::Tag& tag : info.tags) {
            if (tag.original == Catch::StringRef("network") ||
                tag.original == Catch::StringRef("hardware")) {
                rig = true;
            }
        }
        takt4::sandbox::setActive(!rig);
#ifdef _MSC_VER
        _putenv_s("TAKT4_TEST_SANDBOX", rig ? "" : "1");
#else
        if (rig) {
            ::unsetenv("TAKT4_TEST_SANDBOX");
        } else {
            ::setenv("TAKT4_TEST_SANDBOX", "1", 1);
        }
#endif
        for (std::size_t i = 0; i < kKinds.size(); ++i) {
            before_[i] = takt4::sandbox::refusals(kKinds[i]);
        }
    }

    void testCaseEnded(const Catch::TestCaseStats& stats) override {
        // Said, not failed: a refused send is the sandbox working, and most are a target a test
        // never meant to reach. But a test that drove something it thought was real should be
        // able to find out why nothing happened.
        std::string counts;
        for (std::size_t i = 0; i < kKinds.size(); ++i) {
            const std::uint64_t refused = takt4::sandbox::refusals(kKinds[i]) - before_[i];
            if (refused != 0) {
                counts += counts.empty() ? "" : ", ";
                counts += std::to_string(refused) + " " + nameOf(kKinds[i]);
            }
        }
        if (!counts.empty()) {
            std::fprintf(stderr, "[sandbox] \"%s\": refused %s\n", stats.testInfo->name.c_str(),
                         counts.c_str());
        }
    }

private:
    static constexpr std::array<takt4::sandbox::Refused, 5> kKinds = {
        takt4::sandbox::Refused::Send, takt4::sandbox::Refused::Bind,
        takt4::sandbox::Refused::Midi, takt4::sandbox::Refused::Link,
        takt4::sandbox::Refused::Audio};

    static const char* nameOf(takt4::sandbox::Refused what) {
        switch (what) {
        case takt4::sandbox::Refused::Send:
            return "sends";
        case takt4::sandbox::Refused::Bind:
            return "binds";
        case takt4::sandbox::Refused::Midi:
            return "MIDI opens";
        case takt4::sandbox::Refused::Link:
            return "Link joins";
        case takt4::sandbox::Refused::Audio:
            return "audio opens";
        case takt4::sandbox::Refused::Nothing:
            break;
        }
        return "nothing";
    }

    std::array<std::uint64_t, kKinds.size()> before_{};
};

} // namespace

CATCH_REGISTER_LISTENER(RigSandbox)
