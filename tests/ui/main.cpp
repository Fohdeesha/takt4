// takt4_ui_tests' entry point.
//
// A binary of its own, separate from takt4_tests, on purpose. takt4_tests links
// takt4_core and nothing else, which is what proves the engine stands up without Slint
// (HANDOFF §4.2); linking the UI into it would quietly retire that proof. This one links
// takt4::ui and tests the window.
//
// The headless platform has to be installed before any Slint component exists, which is
// why there is a main here rather than Catch2WithMain. It gives the runtime a window
// adapter that never reaches a screen — the same one takt4-shot renders through — so
// these tests run on a build machine, on a CI runner, and in the sessions takt4 is
// developed from, none of which have a display.
//
// Slint's own slint::testing::init() would do the same and would additionally allow
// finding elements and clicking them. It is behind SLINT_FEATURE_EXPERIMENTAL, which this
// build leaves off; everything used here is stable API. What that costs is noted in
// window_test.cpp.

#include "ui/headless.hpp"
#include "ui/shot.hpp"

#include <catch2/catch_session.hpp>

int main(int argc, char** argv) {
    // The window's own preferred size, so any layout the tests provoke is the one an
    // operator would get. Kept rather than discarded: a test that renders the window needs
    // the adapter, and this is the only call that can hand one over. See tests/ui/shot.hpp.
    takt4::tests::headlessPlatform() = takt4::ui::installHeadlessPlatform(900, 520);
    return Catch::Session().run(argc, argv);
}
