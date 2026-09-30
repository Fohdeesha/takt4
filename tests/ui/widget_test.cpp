// The main window's own controls (src/ui/weltformat.slint), driven with real clicks, drags, keys
// and the wheel on the widget bench (src/ui/widget_bench.slint), where each one is at a known
// place and everything it shows or says can be read back.
//
// **What these hold the controls to** is what was learned about the std widgets they
// replace: a control never sets its own value — it says what was asked for, and goes on showing
// what its owner holds; every one of them takes the keyboard on a click, so a box being typed
// into commits; Escape in a box or an open list is never PANIC; the wheel changes nothing; a
// row being driven keeps its element. Each test drives the gesture an operator makes, and
// several turn the bench's owner off (`write-back`) to prove the control did not do the owner's
// job itself.

#include "ui/shot.hpp"

#include "main_window.h" // generated from main_window.slint, which re-exports the bench

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <slint-platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

const std::string kEscape(1, '\x1b');
const std::string kEnter = "\n";
const std::string kBackspace(1, '\b');
const std::string kUp = "\xEF\x9C\x80";    // Key.UpArrow, U+F700
const std::string kDown = "\xEF\x9C\x81";  // Key.DownArrow
const std::string kLeft = "\xEF\x9C\x82";  // Key.LeftArrow
const std::string kRight = "\xEF\x9C\x83"; // Key.RightArrow
const std::string kHome = "\xEF\x9C\xA9";  // Key.Home, U+F729
const std::string kEnd = "\xEF\x9C\xAB";   // Key.End, U+F72B

void settle() {
    slint::platform::update_timers_and_animations();
}

/// Slint's timers and animations for about `span` of real time — a wheel scroll is animated.
void pump(std::chrono::milliseconds span) {
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {
        settle();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    settle();
}

/// The bench, laid out at its own size, with everything it says recorded.
struct Bench {
    slint::ComponentHandle<WidgetBench> ui = WidgetBench::create();
    std::vector<int> clicks;
    std::vector<std::pair<int, bool>> toggles;
    std::vector<std::pair<int, int>> picks;
    std::vector<std::pair<int, std::string>> typed;
    std::vector<std::pair<int, std::string>> committed;
    std::vector<int> escapes;
    std::vector<std::pair<int, std::string>> readings;
    std::vector<float> moves;
    std::vector<float> lows;
    std::vector<float> highs;
    int folds = 0;
    int insides = 0;
    std::vector<std::string> rowEvents;
    std::vector<int> countEdited;
    std::vector<int> countTyped;
    std::vector<float> paintedMoves;

    Bench() {
        ui->on_count_edited([this](int n) { countEdited.push_back(n); });
        ui->on_count_typed([this](int n) { countTyped.push_back(n); });
        ui->on_painted_moved([this](float v) { paintedMoves.push_back(v); });
        ui->on_button_clicked([this](int i) { clicks.push_back(i); });
        ui->on_toggled([this](int i, bool on) { toggles.emplace_back(i, on); });
        ui->on_picked([this](int i, int index) { picks.emplace_back(i, index); });
        ui->on_typed(
            [this](int i, const slint::SharedString& t) { typed.emplace_back(i, std::string(t)); });
        ui->on_committed([this](int i, const slint::SharedString& t) {
            committed.emplace_back(i, std::string(t));
        });
        ui->on_escaped([this](int i) { escapes.push_back(i); });
        ui->on_reading_committed([this](int i, const slint::SharedString& t) {
            readings.emplace_back(i, std::string(t));
        });
        ui->on_track_moved([this](float v) { moves.push_back(v); });
        ui->on_low_moved([this](float v) { lows.push_back(v); });
        ui->on_high_moved([this](float v) { highs.push_back(v); });
        ui->on_fold_clicked([this] { ++folds; });
        ui->on_inside_clicked([this] { ++insides; });
        ui->on_row_toggled([this](int i, bool on) {
            rowEvents.push_back("on " + std::to_string(i) + (on ? " 1" : " 0"));
        });
        ui->on_row_named([this](int i, const slint::SharedString& t) {
            rowEvents.push_back("name " + std::to_string(i) + " " + std::string(t));
        });
        ui->on_row_kind([this](int i, int k) {
            rowEvents.push_back("kind " + std::to_string(i) + " " + std::to_string(k));
        });
        ui->on_row_level([this](int i, float v) {
            rowEvents.push_back("level " + std::to_string(i) + " " +
                                std::to_string(std::lround(v)));
        });

        auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (int i = 1; i <= 30; ++i) {
            names->push_back(slint::SharedString("device " + std::to_string(i)));
        }
        ui->set_long_list(names);

        auto& window = ui->window();
        ui->show();
        window.dispatch_scale_factor_change_event(1.0f);
        window.dispatch_resize_event(slint::LogicalSize({900.0f, 720.0f}));
        window.dispatch_window_active_changed_event(true);
        settle();
    }

    slint::Window& window() { return ui->window(); }

    void move(float x, float y) {
        window().dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
    }
    void down(float x, float y) {
        move(x, y);
        window().dispatch_pointer_press_event(slint::LogicalPosition({x, y}),
                                              slint::PointerEventButton::Left);
    }
    void up(float x, float y) {
        window().dispatch_pointer_release_event(slint::LogicalPosition({x, y}),
                                                slint::PointerEventButton::Left);
    }
    void click(float x, float y) {
        down(x, y);
        up(x, y);
        settle();
    }
    void drag(float x0, float y0, float x1, float y1, int steps) {
        down(x0, y0);
        settle();
        for (int i = 1; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
            settle();
        }
        up(x1, y1);
        settle();
    }
    void key(const std::string& text) {
        window().dispatch_key_press_event(slint::SharedString(text));
        window().dispatch_key_release_event(slint::SharedString(text));
        settle();
    }
    void type(const std::string& text) {
        for (const char c : text) {
            key(std::string(1, c));
        }
    }
    void wheel(float x, float y, float dy) {
        window().dispatch_pointer_scroll_event(slint::LogicalPosition({x, y}), 0.0f, dy);
        settle();
    }
    /// A click on the bench's own background, far from every control.
    void clickAway() { click(860.0f, 480.0f); }
    std::string keysSeen() const { return std::string(ui->get_keys_seen()); }
};

// Where the bench puts things (widget_bench.slint).
constexpr float kButtonY = 38.0f;
float buttonX(int i) {
    return 75.0f + 120.0f * static_cast<float>(i);
}
constexpr float kTickY = 92.0f;
constexpr float kPickY = 147.0f;
constexpr float kEntryY = 207.0f;
constexpr float kTrackY = 262.0f;
constexpr float kTrackLeft = 20.0f;
constexpr float kTrackSpan = 300.0f - 18.0f;
float trackX(float value) {
    return kTrackLeft + 9.0f + kTrackSpan * (value + 100.0f) / 200.0f;
}
constexpr float kSpanLeft = 360.0f;
constexpr float kSpanRange = 360.0f - 18.0f;
float spanX(float bpm) {
    return kSpanLeft + 9.0f + kSpanRange * (bpm - 40.0f) / 180.0f;
}
// The rule editor's: the count and the dashed box at y 530, 34 high; the quiet tick beside them;
// the painted slider at y 580, 0 to 100 over 300 px; the quiet reading at y 620.
constexpr float kCountY = 547.0f;
constexpr float kPaintedY = 592.0f;
float paintedX(float value) {
    return 20.0f + 9.0f + (300.0f - 18.0f) * value / 100.0f;
}
constexpr float kQuietReadingY = 634.0f;

} // namespace

// --- buttons -------------------------------------------------------------------------------

TEST_CASE("a button fires once per click, on its edge too, and a switched-off one never",
          "[ui][widgets]") {
    Bench b;
    for (int i = 0; i < 7; ++i) {
        b.click(buttonX(i), kButtonY);
    }
    CHECK(b.clicks == std::vector<int>{0, 1, 2, 3, 5, 6});

    b.clicks.clear();
    // Inside its 2 px edge, at either end, and one pixel outside it.
    b.click(20.5f, kButtonY);
    b.click(129.5f, kButtonY);
    b.click(20.0f + 110.0f + 3.0f, kButtonY); // the gap between two buttons
    b.click(18.0f, kButtonY);
    CHECK(b.clicks == std::vector<int>{0, 0});
}

TEST_CASE("a button click takes the keyboard and passes the window's shortcuts up",
          "[ui][widgets]") {
    Bench b;
    b.click(buttonX(0), kButtonY);
    b.key("t");
    b.key(kEscape);
    CHECK(b.keysSeen() == "t" + kEscape);
}

TEST_CASE("a button click finishes what was being typed, once", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    REQUIRE(b.ui->get_entry_focused());
    b.key(kEnd);
    b.type("x");
    CHECK(b.committed.empty());
    b.click(buttonX(0), kButtonY);
    settle();
    REQUIRE(b.committed.size() == 1);
    CHECK(b.committed[0] == std::make_pair(0, std::string("deckx")));
    CHECK(b.clicks == std::vector<int>{0});
    CHECK_FALSE(b.ui->get_entry_focused());
}

// --- tick boxes ----------------------------------------------------------------------------

TEST_CASE("a tick box asks for the other state from its box and its words", "[ui][widgets]") {
    Bench b;
    b.click(30.0f, 90.0f);
    b.click(30.0f, 90.0f);
    // The labelled one, by its words well to the right of its box.
    b.click(180.0f, kTickY);
    CHECK(b.toggles == std::vector<std::pair<int, bool>>{{0, true}, {0, false}, {1, true}});
    CHECK_FALSE(b.ui->get_tick_a());
    CHECK(b.ui->get_tick_b());
}

TEST_CASE("a tick box never ticks itself: with no owner writing back it asks again",
          "[ui][widgets]") {
    Bench b;
    b.ui->set_write_back(false);
    b.click(30.0f, 90.0f);
    b.click(30.0f, 90.0f);
    CHECK(b.toggles == std::vector<std::pair<int, bool>>{{0, true}, {0, true}});
    CHECK_FALSE(b.ui->get_tick_a());
    // And when the owner changes it, it shows the owner's state, clicked or not.
    b.ui->set_tick_a(true);
    b.click(30.0f, 90.0f);
    CHECK(b.toggles.back() == std::make_pair(0, false));
}

TEST_CASE("a locked tick box and a switched-off one ignore clicks and Space", "[ui][widgets]") {
    Bench b;
    b.click(310.0f, kTickY);
    b.click(360.0f, kTickY);
    b.click(450.0f, kTickY);
    b.click(500.0f, kTickY);
    b.key(" ");
    CHECK(b.toggles.empty());
}

TEST_CASE("Space ticks a tick box that has the keyboard, and other keys pass up", "[ui][widgets]") {
    Bench b;
    b.click(68.0f, kTickY);
    REQUIRE(b.toggles.size() == 1);
    b.key(" ");
    CHECK(b.toggles.size() == 2);
    CHECK(b.toggles.back() == std::make_pair(1, false));
    b.key("d");
    b.key(kEscape);
    CHECK(b.keysSeen() == "d" + kEscape);
}

TEST_CASE("a tick box click finishes what was being typed", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    b.key(kEnd);
    b.type("y");
    b.click(30.0f, 90.0f);
    settle();
    REQUIRE(b.committed.size() == 1);
    CHECK(b.committed[0].second == "decky");
    CHECK(b.toggles.size() == 1);
}

// --- dropdowns -----------------------------------------------------------------------------

TEST_CASE("a dropdown opens on a click, and Escape closes it without picking or panicking",
          "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kPickY);
    CHECK(b.ui->get_pick_a_open());
    b.key(kEscape);
    CHECK_FALSE(b.ui->get_pick_a_open());
    CHECK(b.picks.empty());
    CHECK(b.keysSeen().empty()); // Escape never reached the window, where it is PANIC
    // And the dropdown still has the keyboard: the next Escape is the window's.
    b.key(kEscape);
    CHECK(b.keysSeen() == kEscape);
}

TEST_CASE("a dropdown's list is walked with the arrows and picked with Enter", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kPickY);
    b.key(kDown);
    b.key(kDown);
    CHECK(b.picks.empty()); // moving the light is not a pick
    b.key(kEnter);
    CHECK_FALSE(b.ui->get_pick_a_open());
    CHECK(b.picks == std::vector<std::pair<int, int>>{{0, 2}});
    CHECK(std::string(b.ui->get_pick_a_shown()) == "third");
    // Up past the first stops there.
    b.click(120.0f, kPickY);
    for (int i = 0; i < 6; ++i) {
        b.key(kUp);
    }
    b.key(kEnter);
    CHECK(b.picks.back() == std::make_pair(0, 0));
}

TEST_CASE("a dropdown's entry is picked with a click, and the one already picked is not a pick",
          "[ui][widgets]") {
    Bench b;
    // The list opens below the box, one 30 px entry after another; found by clicking down it
    // until an entry answers, since where a popup lands is Slint's call.
    int found = -1;
    for (float y = kPickY + 20.0f; y < 500.0f && found < 0; y += 6.0f) {
        b.click(120.0f, kPickY);
        REQUIRE(b.ui->get_pick_a_open());
        b.click(120.0f, y);
        if (!b.picks.empty()) {
            found = b.picks.back().second;
        }
    }
    INFO("an entry answered a click at index " << found);
    REQUIRE(found > 0);
    CHECK_FALSE(b.ui->get_pick_a_open());
    CHECK(std::string(b.ui->get_pick_a_shown()) ==
          std::string(*b.ui->get_short_list()->row_data(static_cast<std::size_t>(found))));

    // The one it shows, picked again: nothing is said.
    const std::size_t before = b.picks.size();
    b.click(120.0f, kPickY);
    b.key(kEnter); // Enter on the light, which opens on what is picked
    CHECK(b.picks.size() == before);
}

TEST_CASE("a focused dropdown steps with the arrows without opening, and stops at the ends",
          "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kPickY);
    b.key(kEscape);
    REQUIRE(b.ui->get_pick_a_focused());
    b.key(kDown);
    b.key(kDown);
    CHECK_FALSE(b.ui->get_pick_a_open());
    CHECK(b.picks == std::vector<std::pair<int, int>>{{0, 1}, {0, 2}});
    b.key(kUp);
    b.key(kUp);
    b.key(kUp);
    CHECK(b.picks.back() == std::make_pair(0, 0));
    CHECK(b.picks.size() == 4); // the third Up, at the top, said nothing
    for (int i = 0; i < 8; ++i) {
        b.key(kDown);
    }
    CHECK(b.picks.back() == std::make_pair(0, 4));
    CHECK(b.picks.size() == 8); // four steps down, and nothing past the end
}

TEST_CASE("a dropdown never picks for itself: with no owner writing back it shows the owner's",
          "[ui][widgets]") {
    Bench b;
    b.ui->set_write_back(false);
    b.click(120.0f, kPickY);
    b.key(kDown);
    b.key(kEnter);
    CHECK(b.picks == std::vector<std::pair<int, int>>{{0, 1}});
    CHECK(std::string(b.ui->get_pick_a_shown()) == "first");
    // The owner's index moves it, and a new list keeps the owner's index.
    b.ui->set_pick_a(3);
    CHECK(std::string(b.ui->get_pick_a_shown()) == "fourth");
    auto other = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* s : {"a", "b", "c", "d", "e", "f"}) {
        other->push_back(slint::SharedString(s));
    }
    b.ui->set_short_list(other);
    CHECK(std::string(b.ui->get_pick_a_shown()) == "d");
    CHECK(b.picks.size() == 1); // a new list is not a pick
}

TEST_CASE("the wheel over a dropdown changes nothing, focused or not", "[ui][widgets]") {
    Bench b;
    b.wheel(120.0f, kPickY, -120.0f);
    b.click(120.0f, kPickY);
    b.key(kEscape);
    REQUIRE(b.ui->get_pick_a_focused());
    b.wheel(120.0f, kPickY, -120.0f);
    b.wheel(120.0f, kPickY, 120.0f);
    CHECK(b.picks.empty());
    CHECK(std::string(b.ui->get_pick_a_shown()) == "first");
}

TEST_CASE("a locked dropdown and a switched-off one neither open nor step", "[ui][widgets]") {
    Bench b;
    for (const float x : {500.0f, 680.0f}) {
        b.click(x, kPickY);
        b.key(kDown);
        b.key(kEnter);
        b.key(" ");
    }
    CHECK(b.picks.empty());
    // The keys went to the window, as they would from nothing at all.
    CHECK(b.keysSeen() == kDown + kEnter + " " + kDown + kEnter + " ");
}

TEST_CASE("a long list opens on the entry picked and scrolls to reach the rest", "[ui][widgets]") {
    Bench b;
    b.click(320.0f, kPickY);
    REQUIRE(b.ui->get_pick_long_open());
    for (int i = 0; i < 25; ++i) {
        b.key(kDown);
    }
    b.key(kEnter);
    CHECK(b.picks == std::vector<std::pair<int, int>>{{1, 25}});
    CHECK(std::string(b.ui->get_pick_long_shown()) == "device 26");
    // Opened again, the light starts on 25: one step up is 24.
    b.click(320.0f, kPickY);
    b.key(kUp);
    b.key(kEnter);
    CHECK(b.picks.back() == std::make_pair(1, 24));
}

// --- text boxes ----------------------------------------------------------------------------

TEST_CASE("a text box says every keystroke and commits once, on Enter", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    b.key(kEnd);
    for (int i = 0; i < 4; ++i) {
        b.key(kBackspace);
    }
    b.type("wall");
    CHECK(b.typed.size() == 8);
    CHECK(b.typed.back() == std::make_pair(0, std::string("wall")));
    CHECK(std::string(b.ui->get_entry_shown()) == "wall");
    CHECK(b.committed.empty()); // nothing applied a keystroke at a time
    b.key(kEnter);
    settle();
    CHECK(b.committed == std::vector<std::pair<int, std::string>>{{0, "wall"}});
    CHECK_FALSE(b.ui->get_entry_focused());
    // The focus leaving because of the Enter is not a second edit.
    b.clickAway();
    CHECK(b.committed.size() == 1);
    // And the keyboard went somewhere that passes shortcuts up.
    b.key("m");
    CHECK(b.keysSeen() == "m");
}

TEST_CASE("a text box commits on a click away, once", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    b.key(kEnd);
    b.type("s");
    b.clickAway();
    settle();
    CHECK(b.committed == std::vector<std::pair<int, std::string>>{{0, "decks"}});
    b.clickAway();
    CHECK(b.committed.size() == 1);
}

TEST_CASE("Escape in a text box finishes the edit and is never PANIC", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    b.key(kEnd);
    b.type("q");
    b.key(kEscape);
    settle();
    CHECK(b.escapes == std::vector<int>{0});
    CHECK(b.committed == std::vector<std::pair<int, std::string>>{{0, "deckq"}});
    CHECK(b.keysSeen().empty());
    CHECK_FALSE(b.ui->get_entry_focused());
    // Out of the box, Escape is the window's again.
    b.key(kEscape);
    CHECK(b.keysSeen() == kEscape);
}

TEST_CASE("Escape in a box that puts back what it held commits nothing", "[ui][widgets]") {
    Bench b;
    b.click(300.0f, kEntryY);
    b.key(kEnd);
    b.type("9");
    CHECK(std::string(b.ui->get_revert_shown()) == "70019");
    b.key(kEscape);
    settle();
    CHECK(b.committed.empty());
    CHECK(b.escapes == std::vector<int>{1});
    CHECK(std::string(b.ui->get_revert_shown()) == "7001");
    CHECK(b.keysSeen().empty());
}

TEST_CASE("letters typed into a box are the box's, not the window's shortcuts", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    b.type("tdm TDM");
    CHECK(b.keysSeen().empty());
}

TEST_CASE("a text box follows its value while not typed in, and never over what is typed",
          "[ui][widgets]") {
    Bench b;
    b.ui->set_entry_value("wall");
    settle();
    CHECK(std::string(b.ui->get_entry_shown()) == "wall");
    b.click(120.0f, kEntryY);
    b.key(kEnd);
    b.type("s");
    b.ui->set_entry_value("robot"); // the owner changes it under the typing
    settle();
    CHECK(std::string(b.ui->get_entry_shown()) == "walls");
    // Finished without an owner writing it back: the box shows what the owner holds.
    b.ui->set_write_back(false);
    b.key(kEnter);
    settle();
    CHECK(b.committed.back().second == "walls");
    CHECK(std::string(b.ui->get_entry_shown()) == "robot");
    // And after an edit — which is what drops a binding — it still follows its owner.
    b.ui->set_entry_value("stage");
    settle();
    CHECK(std::string(b.ui->get_entry_shown()) == "stage");
}

TEST_CASE("clicking from one box into another commits the first to itself", "[ui][widgets]") {
    Bench b;
    b.click(120.0f, kEntryY);
    b.key(kEnd);
    b.type("1");
    b.click(300.0f, kEntryY);
    settle();
    CHECK(b.committed == std::vector<std::pair<int, std::string>>{{0, "deck1"}});
    CHECK(b.ui->get_revert_focused());
    b.key(kEnd);
    b.type("2");
    b.key(kEnter);
    settle();
    CHECK(b.committed.back() == std::make_pair(1, std::string("70012")));
    CHECK(std::string(b.ui->get_entry_value()) == "deck1");
}

// --- the typable reading -------------------------------------------------------------------

TEST_CASE("a reading opens a box on a click, takes a number on Enter, and puts it back on Escape",
          "[ui][widgets]") {
    Bench b;
    b.click(409.0f, kEntryY);
    REQUIRE(b.ui->get_reading_editing());
    b.type("34"); // everything in the box was selected, so this replaces it
    b.key(kEnter);
    settle();
    CHECK(b.readings == std::vector<std::pair<int, std::string>>{{0, "34"}});
    CHECK_FALSE(b.ui->get_reading_editing());
    CHECK(std::string(b.ui->get_reading_value()) == "34");

    b.click(409.0f, kEntryY);
    b.type("99");
    b.key(kEscape);
    settle();
    CHECK(b.readings.size() == 1);
    CHECK_FALSE(b.ui->get_reading_editing());
    CHECK(b.keysSeen().empty());

    b.click(409.0f, kEntryY);
    b.type("-5");
    b.clickAway();
    settle();
    CHECK(b.readings.back() == std::make_pair(0, std::string("-5")));
    CHECK(b.readings.size() == 2);
}

TEST_CASE("a switched-off reading does not open", "[ui][widgets]") {
    Bench b;
    b.click(489.0f, kEntryY);
    b.type("5");
    b.key(kEnter);
    CHECK(b.readings.empty());
    CHECK(b.keysSeen() == "5" + kEnter);
}

// --- sliders -------------------------------------------------------------------------------

TEST_CASE("a slider jumps to a press on its track and follows the drag", "[ui][widgets]") {
    Bench b;
    b.click(trackX(-50.0f), kTrackY);
    REQUIRE_FALSE(b.moves.empty());
    CHECK(b.moves.back() == Catch::Approx(-50.0f).margin(1.0f));
    CHECK(b.ui->get_track_value() == Catch::Approx(-50.0f).margin(1.0f));
    CHECK(static_cast<float>(b.ui->get_track_handle()) + kTrackLeft ==
          Catch::Approx(trackX(b.ui->get_track_value())).margin(0.6f));

    // Grabbed by its handle, a press does not move it; the drag does, by what it covers.
    b.moves.clear();
    const float from = trackX(b.ui->get_track_value());
    b.down(from + 3.0f, kTrackY);
    settle();
    CHECK(b.moves.empty());
    for (int i = 1; i <= 10; ++i) {
        b.move(from + 3.0f + static_cast<float>(i) * 14.1f, kTrackY);
        settle();
    }
    b.up(from + 3.0f + 141.0f, kTrackY);
    settle();
    CHECK(b.moves.size() == 10);
    CHECK(b.ui->get_track_value() == Catch::Approx(-50.0f + 100.0f).margin(1.0f));
}

TEST_CASE("a slider stops at its ends however far it is dragged", "[ui][widgets]") {
    Bench b;
    b.drag(trackX(0.0f), kTrackY, 890.0f, kTrackY, 20);
    CHECK(b.ui->get_track_value() == 100.0f);
    b.drag(trackX(100.0f), kTrackY, 2.0f, kTrackY, 20);
    CHECK(b.ui->get_track_value() == -100.0f);
    for (const float v : b.moves) {
        CHECK(v >= -100.0f);
        CHECK(v <= 100.0f);
    }
}

TEST_CASE("a slider never moves itself: with no owner writing back its handle stays",
          "[ui][widgets]") {
    Bench b;
    b.ui->set_write_back(false);
    const float handle = b.ui->get_track_handle();
    b.drag(trackX(0.0f), kTrackY, trackX(0.0f) + 100.0f, kTrackY, 10);
    REQUIRE(b.moves.size() == 10);
    for (std::size_t i = 1; i < b.moves.size(); ++i) {
        CHECK(b.moves[i] > b.moves[i - 1]); // each step says where the hand is, not a nudge
    }
    CHECK(b.ui->get_track_value() == 0.0f);
    CHECK(b.ui->get_track_handle() == handle);
}

TEST_CASE("a switched-off slider ignores the pointer and the keys", "[ui][widgets]") {
    Bench b;
    b.ui->set_track_on(false);
    b.drag(trackX(0.0f), kTrackY, trackX(60.0f), kTrackY, 5);
    b.key(kRight);
    CHECK(b.moves.empty());
}

TEST_CASE("a slider steps with the arrows and goes to its ends with Home and End",
          "[ui][widgets]") {
    Bench b;
    b.click(trackX(0.0f), kTrackY); // on the handle: takes the keyboard, moves nothing
    CHECK(b.moves.empty());
    b.key(kRight);
    b.key(kRight);
    b.key(kLeft);
    CHECK(b.ui->get_track_value() == 1.0f);
    b.key(kEnd);
    CHECK(b.ui->get_track_value() == 100.0f);
    b.key(kHome);
    CHECK(b.ui->get_track_value() == -100.0f);
    // Keys it has no use for are the window's.
    b.key("t");
    CHECK(b.keysSeen() == "t");
}

TEST_CASE("the wheel over a slider changes nothing", "[ui][widgets]") {
    Bench b;
    b.click(trackX(0.0f), kTrackY);
    b.wheel(trackX(0.0f), kTrackY, 120.0f);
    b.wheel(trackX(0.0f), kTrackY, -120.0f);
    CHECK(b.moves.empty());
    CHECK(b.ui->get_track_value() == 0.0f);
}

TEST_CASE("a two-handled slider moves the nearer end and never lets the two cross",
          "[ui][widgets]") {
    Bench b;
    // The low end, dragged far past the high one.
    b.drag(spanX(70.0f), kTrackY, spanX(200.0f), kTrackY, 30);
    CHECK(b.highs.empty());
    CHECK(b.ui->get_span_low() == 135.0f); // five short of 140
    for (const float v : b.lows) {
        CHECK(v <= 135.0f);
    }
    // The high end, dragged far past the low one: it is nearer to the press on it, even with
    // the low end five BPM away.
    b.lows.clear();
    b.drag(spanX(140.0f) + 4.0f, kTrackY, spanX(50.0f), kTrackY, 30);
    CHECK(b.lows.empty());
    CHECK(b.ui->get_span_high() == 140.0f); // it cannot go below 135 + 5
    // A press on the track beyond either end takes that end there.
    b.click(spanX(200.0f), kTrackY);
    CHECK(b.ui->get_span_high() == Catch::Approx(200.0f).margin(1.0f));
    b.click(spanX(50.0f), kTrackY);
    CHECK(b.ui->get_span_low() == Catch::Approx(50.0f).margin(1.0f));
    CHECK(b.ui->get_span_high() - b.ui->get_span_low() >= 5.0f);
}

TEST_CASE("a two-handled slider with its handles over each other takes the side pressed",
          "[ui][widgets]") {
    Bench b;
    b.ui->set_span_low(100.0f);
    b.ui->set_span_high(105.0f);
    settle();
    const float low = spanX(100.0f);
    const float high = spanX(105.0f);
    REQUIRE(high - low < 18.0f); // the handles, 18 px round, overlap
    b.drag(low - 5.0f, kTrackY, low - 45.0f, kTrackY, 8);
    CHECK_FALSE(b.lows.empty());
    CHECK(b.highs.empty());
    CHECK(b.ui->get_span_low() < 100.0f);
    b.drag(high + 5.0f, kTrackY, high + 45.0f, kTrackY, 8);
    CHECK_FALSE(b.highs.empty());
    CHECK(b.ui->get_span_high() > 105.0f);
}

// --- a section that folds, and the scroller -----------------------------------------------

TEST_CASE("a heading's fold arrow folds and unfolds", "[ui][widgets]") {
    Bench b;
    b.click(506.0f, 314.0f);
    CHECK(b.folds == 1);
    CHECK(b.ui->get_folded());
    b.click(506.0f, 314.0f);
    CHECK(b.folds == 2);
    CHECK_FALSE(b.ui->get_folded());
    // The heading's words are not the arrow.
    b.click(80.0f, 314.0f);
    CHECK(b.folds == 2);
}

TEST_CASE("the scroller scrolls on the wheel and by its bar, and keeps in range as its content "
          "shrinks",
          "[ui][widgets]") {
    Bench b;
    CHECK(b.ui->get_scrolled() == 0.0f);
    b.click(90.0f, 386.0f);
    CHECK(b.insides == 1);
    // The bar — 8 px wide, 1 px in from the right, 35.5 px long over 600 px of content in a
    // 150 px view — dragged to the bottom.
    b.drag(314.0f, 370.0f, 314.0f, 700.0f, 10);
    CHECK(b.ui->get_scrolled() == Catch::Approx(600.0f - 150.0f).margin(1.0f));
    // The button that was under the pointer has scrolled away: the same click reaches nothing.
    b.click(90.0f, 386.0f);
    CHECK(b.insides == 1);
    // The wheel, over the content, back up; Slint animates a wheel scroll.
    b.wheel(170.0f, 450.0f, 60.0f);
    pump(std::chrono::milliseconds(300));
    CHECK(b.ui->get_scrolled() < 450.0f);
    CHECK(b.ui->get_scrolled() >= 0.0f);
    // The content made shorter than the view: nothing left to scroll, and nothing scrolled.
    b.ui->set_scroll_content(120.0f);
    settle();
    CHECK(b.ui->get_scrolled() == 0.0f);
    b.click(90.0f, 386.0f);
    CHECK(b.insides == 2);
}

TEST_CASE("the scroller's bar is drawn 8 px wide, is grabbed 15 px wide, and a click on its track "
          "moves a page",
          "[ui][widgets]") {
    // The operator: the first 6 px bar was "super small hard to grab". The bench's scroller runs
    // x 20 to 320; its bar is drawn at 311 to 318 and taken anywhere from 305 to 319.
    Bench b;
    const takt4::tests::Shot shot = takt4::tests::render(*b.ui, 900, 720);
    b.window().dispatch_window_active_changed_event(true);
    int first = -1;
    int last = -1;
    for (int x = 290; x < 330; ++x) {
        if (shot.is(x, 370, 0x55, 0x55, 0x53)) {
            first = first < 0 ? x : first;
            last = x;
        }
    }
    CHECK(first == 311);
    CHECK(last == 318);

    // Grabbed 6 px left of what is drawn, over the content's own edge, and dragged to the end.
    b.drag(305.0f, 370.0f, 305.0f, 700.0f, 10);
    CHECK(b.ui->get_scrolled() == Catch::Approx(450.0f).margin(1.0f));
    // And from its right-hand pixel back to the top.
    b.drag(319.0f, 490.0f, 319.0f, 300.0f, 10);
    CHECK(b.ui->get_scrolled() == Catch::Approx(0.0f).margin(1.0f));

    // The track below the bar: a page down — the 150 px view less 40, so a line stays in sight.
    b.click(314.0f, 480.0f);
    CHECK(b.ui->get_scrolled() == Catch::Approx(110.0f).margin(0.5f));
    b.click(314.0f, 480.0f);
    CHECK(b.ui->get_scrolled() == Catch::Approx(220.0f).margin(0.5f));
    // A click on the bar itself, now at 406 to 441, moves nothing.
    b.click(314.0f, 424.0f);
    CHECK(b.ui->get_scrolled() == Catch::Approx(220.0f).margin(0.5f));
    // Above it, a page up; and never past either end.
    b.click(314.0f, 395.0f);
    CHECK(b.ui->get_scrolled() == Catch::Approx(110.0f).margin(0.5f));
    b.click(314.0f, 352.0f);
    b.click(314.0f, 352.0f);
    CHECK(b.ui->get_scrolled() == Catch::Approx(0.0f).margin(0.5f));
    for (int i = 0; i < 6; ++i) {
        b.click(314.0f, 498.0f);
    }
    CHECK(b.ui->get_scrolled() == Catch::Approx(450.0f).margin(0.5f));

    // A slow drag, a pixel at a time: at every step the view is where the pointer says — 450 px
    // of scrolling over the 106.5 px the bar can travel — and never somewhere the last step put it.
    b.down(314.0f, 478.0f);
    settle();
    for (int i = 1; i <= 20; ++i) {
        b.move(314.0f, 478.0f - static_cast<float>(i));
        settle();
        INFO("step " << i);
        CHECK(b.ui->get_scrolled() ==
              Catch::Approx(450.0f - static_cast<float>(i) * 450.0f / 106.5f).margin(0.5f));
    }
    b.up(314.0f, 458.0f);
    settle();

    // Nothing under the track was pressed on the way.
    CHECK(b.clicks.empty());
    CHECK(b.insides == 0);
}

// --- rows: a repeater being driven keeps its elements -------------------------------------

namespace {

std::shared_ptr<slint::VectorModel<BenchRow>> benchRows(Bench& b) {
    auto rows = std::make_shared<slint::VectorModel<BenchRow>>();
    rows->push_back(BenchRow{slint::SharedString("deck"), true, 0, 0.0f});
    rows->push_back(BenchRow{slint::SharedString("wall"), true, 3, -40.0f});
    b.ui->set_rows(rows);
    settle();
    return rows;
}

constexpr float kRow0Y = 557.0f;

} // namespace

TEST_CASE("a row's open list survives its row changing under it", "[ui][widgets]") {
    Bench b;
    const auto rows = benchRows(b);
    b.click(582.0f, kRow0Y);
    // Its row rewritten in place, as the window's controller writes rows, while the list is open.
    for (int i = 0; i < 5; ++i) {
        BenchRow row = *rows->row_data(0);
        row.name = slint::SharedString("deck " + std::to_string(i));
        row.level = static_cast<float>(i);
        rows->set_row_data(0, row);
        settle();
    }
    // Two steps down the open list and Enter picks the third kind. Had the list been destroyed,
    // the same keys would step the closed dropdown (kind 1, twice) or reach nothing at all.
    b.key(kDown);
    b.key(kDown);
    b.key(kEnter);
    CHECK(b.rowEvents == std::vector<std::string>{"kind 0 2"});
}

TEST_CASE("a row's box being typed in keeps what is typed while its row changes, and commits it "
          "to that row",
          "[ui][widgets]") {
    Bench b;
    const auto rows = benchRows(b);
    b.click(456.0f, kRow0Y);
    b.key(kEnd);
    b.type("x");
    BenchRow row = *rows->row_data(0);
    row.name = slint::SharedString("renamed from outside");
    row.level = 50.0f;
    row.on = false;
    rows->set_row_data(0, row);
    settle();
    b.key(kEnter);
    settle();
    CHECK(b.rowEvents == std::vector<std::string>{"name 0 deckx"});
}

TEST_CASE("a row's tick shows its row, clicked or not", "[ui][widgets]") {
    Bench b;
    const auto rows = benchRows(b);
    // Clicked with nobody writing back: it asks for off, twice, because the row still says on.
    b.click(375.0f, kRow0Y);
    b.click(375.0f, kRow0Y);
    CHECK(b.rowEvents == std::vector<std::string>{"on 0 0", "on 0 0"});
    // The row switched off from outside: the next click asks for on.
    BenchRow row = *rows->row_data(0);
    row.on = false;
    rows->set_row_data(0, row);
    settle();
    b.click(375.0f, kRow0Y);
    CHECK(b.rowEvents.back() == "on 0 1");
}

TEST_CASE("a row's slider is dragged across its row being rewritten on every step",
          "[ui][widgets]") {
    // The output rows' delay sliders: the controller writes the row back on every step of a drag.
    // Rewritten in place the drag goes on following the hand; rebuilt, it stopped at the first
    // pixel (the audit of 2026-09-25, T11).
    Bench b;
    const auto rows = benchRows(b);
    b.ui->on_row_level([&](int i, float v) {
        BenchRow row = *rows->row_data(static_cast<std::size_t>(i));
        row.level = v;
        rows->set_row_data(static_cast<std::size_t>(i), row);
        b.rowEvents.push_back("level " + std::to_string(i) + " " + std::to_string(std::lround(v)));
    });
    // The slider in row 0 runs from 648 to 880: its handle at 0 is in the middle.
    const float middle = 648.0f + 9.0f + (880.0f - 648.0f - 18.0f) / 2.0f;
    b.drag(middle, kRow0Y, middle + 80.0f, kRow0Y, 40);
    REQUIRE(b.rowEvents.size() >= 30);
    CHECK(rows->row_data(0)->level > 60.0f);
}

// --- the rule editor's: the count, the dashed box, and the quiet controls ---------------------

TEST_CASE("a count takes a whole number, clamped to its range, and never sets itself",
          "[ui][widgets]") {
    Bench b;
    // Into the box, emptied, and typed into; Enter hands the keyboard back to the window, so every
    // edit starts with a click.
    const auto retype = [&b](const std::string& text) {
        b.click(70.0f, kCountY);
        REQUIRE(b.ui->get_count_focused());
        b.key(kEnd);
        b.key(kBackspace);
        b.key(kBackspace);
        b.type(text);
    };
    retype("9");
    // Every keystroke says what Enter would set — the value it has while the box holds no number.
    CHECK(b.countTyped == std::vector<int>{4, 9});
    CHECK(b.countEdited.empty());
    b.key(kEnter);
    CHECK(b.countEdited == std::vector<int>{9});
    CHECK(b.ui->get_count_value() == 9);

    // Past its end, held to it; not a number, nothing, and the box shows the count again.
    retype("40");
    b.key(kEnter);
    CHECK(b.countEdited.back() == 16);
    retype("x");
    b.key(kEnter);
    CHECK(b.countEdited.size() == 2);
    CHECK(std::string(b.ui->get_count_shown()) == "16");

    // The arrows step it and stop at the ends: up from 16 is nothing, down is 15, said and set.
    b.click(70.0f, kCountY);
    b.countTyped.clear();
    b.key(kUp);
    CHECK(b.countEdited.size() == 2);
    b.key(kDown);
    CHECK(b.countTyped == std::vector<int>{15});
    CHECK(b.countEdited.back() == 15);
    CHECK(std::string(b.ui->get_count_shown()) == "15");
    CHECK(b.ui->get_count_focused()); // a step keeps the keyboard

    // Escape puts it back, says so to whoever kept what was typed, and is never PANIC.
    retype("3");
    b.key(kEscape);
    CHECK(b.countTyped.back() == 15);
    CHECK(b.countEdited.back() == 15);
    CHECK(std::string(b.ui->get_count_shown()) == "15");
    CHECK(b.keysSeen().empty());

    // With no owner writing back, it asks and goes on showing the owner's.
    b.ui->set_write_back(false);
    retype("7");
    b.key(kEnter);
    CHECK(b.countEdited.back() == 7);
    CHECK(b.ui->get_count_value() == 15);
    CHECK(std::string(b.ui->get_count_shown()) == "15");
}

TEST_CASE("a dashed box is still a box: it takes the keyboard, and draws solid while typed in",
          "[ui][widgets]") {
    // B's BPM box while B is off, A's cooldown box and an empty channel: dashed to say the value
    // is set aside, and still typed into.
    Bench b;
    // The top edge, where the dashes are. Dashed, some of it is the box's own fill showing
    // between them; being typed in, it is one colour end to end — the focus edge, solid.
    const auto topEdge = [&b] {
        const takt4::tests::Shot shot = takt4::tests::render(*b.ui, 900, 720);
        std::vector<slint::Rgb8Pixel> edge;
        for (int x = 104; x < 196; ++x) {
            edge.push_back(shot.at(x, 530));
        }
        return std::make_pair(edge, shot.at(190, 540));
    };
    const auto same = [](slint::Rgb8Pixel a, slint::Rgb8Pixel c) {
        return a.r == c.r && a.g == c.g && a.b == c.b;
    };
    const auto count = [&same](const std::vector<slint::Rgb8Pixel>& edge, slint::Rgb8Pixel like) {
        return std::count_if(edge.begin(), edge.end(),
                             [&](slint::Rgb8Pixel p) { return same(p, like); });
    };
    const auto dashed = topEdge();
    CHECK(count(dashed.first, dashed.second) > 20);
    b.click(180.0f, kCountY);
    REQUIRE(b.ui->get_dashed_focused());
    const auto typing = topEdge();
    CHECK(count(typing.first, typing.first.front()) ==
          static_cast<std::ptrdiff_t>(typing.first.size()));
    CHECK_FALSE(same(typing.first.front(), typing.second));
    b.key(kEnd);
    for (int i = 0; i < 3; ++i) {
        b.key(kBackspace);
    }
    b.type("120");
    b.key(kEnter);
    CHECK(b.committed == std::vector<std::pair<int, std::string>>{{2, "120"}});
    CHECK(std::string(b.ui->get_dashed_value()) == "120");
}

TEST_CASE("a quiet tick, slider and reading are drawn off and still answer the hand",
          "[ui][widgets]") {
    // B while its switch is off: what it holds is set for later, so the controls look switched
    // off and still work.
    Bench b;
    b.click(290.0f, kCountY); // the tick's word
    CHECK(b.toggles == std::vector<std::pair<int, bool>>{{4, true}});
    CHECK(b.ui->get_tick_quiet());

    b.click(paintedX(20.0f), kPaintedY);
    REQUIRE_FALSE(b.paintedMoves.empty());
    CHECK(b.ui->get_painted_value() == Catch::Approx(20.0f).margin(1.0f));
    b.drag(paintedX(20.0f), kPaintedY, paintedX(70.0f), kPaintedY, 10);
    CHECK(b.ui->get_painted_value() == Catch::Approx(70.0f).margin(1.0f));

    b.click(49.0f, kQuietReadingY);
    REQUIRE(b.ui->get_quiet_reading_editing());
    b.type("0.5");
    b.key(kEnter);
    CHECK(b.readings == std::vector<std::pair<int, std::string>>{{2, "0.5"}});
    CHECK(std::string(b.ui->get_quiet_reading_value()) == "0.5");
}

TEST_CASE("the code face draws f and f, never the font's ff ligature", "[ui][widgets]") {
    // The rule editor sets colour codes in Chivo Mono, and "#20ff80" has to read f, f: the font's
    // own `liga` joins them, and Slint cannot switch a feature off, so our copy of the font has no
    // `liga` (tools/drop_font_feature.py). The bench draws "ff" beside "f" and " f" in the same
    // face at the same size; the pair has to be those two, one over the other.
    Bench b;
    const takt4::tests::Shot shot = takt4::tests::render(*b.ui, 900, 720);
    const auto ink = [&shot](int x, int y) {
        const slint::Rgb8Pixel p = shot.at(x, y);
        return static_cast<int>(std::max({p.r, p.g, p.b}));
    };
    int inked = 0;
    int differ = 0;
    for (int dy = 0; dy < 60; ++dy) {
        for (int dx = -8; dx < 72; ++dx) {
            const int pair = ink(100 + dx, 650 + dy);
            const int both = std::max(ink(180 + dx, 650 + dy), ink(260 + dx, 650 + dy));
            inked += pair > 128 || both > 128 ? 1 : 0;
            differ += std::abs(pair - both) > 64 ? 1 : 0;
        }
    }
    INFO(differ << " of " << inked << " inked pixels differ");
    CHECK(inked > 200); // something was drawn to compare
    CHECK(differ * 50 < inked);
}
