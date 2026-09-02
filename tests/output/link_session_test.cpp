#include "core/output/link_session.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;

TEST_CASE("Link session starts disabled at the requested tempo", "[link]") {
    const takt4::output::LinkSession session(120.0);

    CHECK_FALSE(session.enabled());
    CHECK(session.numPeers() == 0);
    CHECK_THAT(session.tempoBpm(), WithinAbs(120.0, 1e-9));
}

TEST_CASE("Link sessions can be created and destroyed repeatedly", "[link]") {
    for (int i = 0; i < 3; ++i) {
        const takt4::output::LinkSession session(90.0 + i);
        CHECK_THAT(session.tempoBpm(), WithinAbs(90.0 + i, 1e-9));
    }
}
