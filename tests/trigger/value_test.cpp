#include "core/trigger/value.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

using Catch::Approx;
using takt4::trigger::Value;

namespace {

std::string spelled(const Value& value) {
    std::string out;
    value.appendTo(out);
    return out;
}

} // namespace

TEST_CASE("a value carries what the transports underneath can", "[trigger][value]") {
    // §5.8's Fixed generator: "Literal int, float, string or bool" — which is also what
    // OSC 1.0 as output::OscMessage encodes, plus MIDI's integers.
    CHECK(Value{}.kind() == Value::Kind::Int);
    CHECK(Value{}.asInt() == 0);

    const Value number = Value::ofInt(-42);
    CHECK(number.kind() == Value::Kind::Int);
    CHECK(number.asInt() == -42);
    CHECK(number.asFloat() == Approx(-42.0f));
    CHECK(number.asBool());
    CHECK(spelled(number) == "-42");
    CHECK(Value::ofInt(0).asBool() == false);

    const Value fraction = Value::ofFloat(0.5f);
    CHECK(fraction.kind() == Value::Kind::Float);
    CHECK(fraction.asFloat() == Approx(0.5f));
    CHECK(spelled(fraction) == "0.5");
    // A whole number reads as one: an operator who set a layer to 4 wants "4" in the
    // address, not "4.000000".
    CHECK(spelled(Value::ofFloat(4.0f)) == "4");

    const Value flag = Value::ofBool(true);
    CHECK(flag.kind() == Value::Kind::Bool);
    CHECK(flag.asInt() == 1);
    CHECK(spelled(flag) == "1");
    CHECK(spelled(Value::ofBool(false)) == "0");

    const Value name = Value::ofText("clip");
    CHECK(name.kind() == Value::Kind::Text);
    CHECK(name.text() == "clip");
    CHECK(spelled(name) == "clip");
    CHECK(name.asBool());
    CHECK(Value::ofText("").asBool() == false);
    CHECK(Value::ofInt(3).text().empty()); // text() is for text; appendTo spells them all
}

TEST_CASE("every conversion answers, because a rule that sends nothing is worse",
          "[trigger][value]") {
    // Total on purpose: an operator who picks a float generator for a MIDI note should get
    // a note, not silence. See the accessors' note.
    CHECK(Value::ofFloat(3.4f).asInt() == 3);
    CHECK(Value::ofFloat(3.6f).asInt() == 4);
    CHECK(Value::ofFloat(-3.6f).asInt() == -4);
    CHECK(Value::ofText("64").asInt() == 64);
    CHECK(Value::ofText("64").asFloat() == Approx(64.0f));
    CHECK(Value::ofText("not a number").asInt() == 0);
    CHECK(Value::ofText("12abc").asInt() == 12);

    SECTION("a float too large for an int saturates rather than wrapping") {
        // A MIDI note built from a wrapped negative is a note nobody asked for.
        CHECK(Value::ofFloat(1e30f).asInt() == 2147483647);
        CHECK(Value::ofFloat(-1e30f).asInt() == -2147483647 - 1);
    }

    SECTION("a float that is not a number is zero, not undefined") {
        CHECK(Value::ofFloat(std::numeric_limits<float>::quiet_NaN()).asInt() == 0);
        CHECK(Value::ofFloat(std::numeric_limits<float>::infinity()).asInt() == 0);
    }
}

TEST_CASE("text longer than a value holds is truncated, not refused", "[trigger][value]") {
    // An address segment that long is a mistake, and a rule that quietly sends a shorter
    // one is less bad than a rule that stops firing. `truncated()` is how a UI says so.
    const std::string tooLong(Value::kTextCapacity + 10, 'x');
    const Value cut = Value::ofText(tooLong);
    CHECK(cut.truncated());
    CHECK(cut.text().size() == Value::kTextCapacity);
    CHECK(cut.text() == std::string(Value::kTextCapacity, 'x'));

    const Value exact = Value::ofText(std::string(Value::kTextCapacity, 'y'));
    CHECK_FALSE(exact.truncated());
    CHECK(exact.text().size() == Value::kTextCapacity);
}

TEST_CASE("two values are equal when an operator would call them the same", "[trigger][value]") {
    // What the no-repeat guard compares, so this has to mean "the same clip" rather than
    // "the same bytes": a guard that thought 3 and 3.0 were different would let a repeat
    // through in exactly the case it exists for.
    CHECK(Value::ofInt(3) == Value::ofInt(3));
    CHECK(Value::ofInt(3) != Value::ofInt(4));
    CHECK(Value::ofText("a") == Value::ofText("a"));
    CHECK(Value::ofText("a") != Value::ofText("b"));
    CHECK(Value::ofBool(true) == Value::ofBool(true));
    CHECK(Value::ofBool(true) != Value::ofBool(false));

    // Different kinds are different values, and deliberately so: a generator produces one
    // kind throughout, so a guard never compares across them, and treating an int 1 as
    // equal to the text "1" would be a rule about formatting rather than about content.
    CHECK(Value::ofInt(1) != Value::ofBool(true));
    CHECK(Value::ofInt(1) != Value::ofText("1"));
    CHECK(Value::ofInt(1) != Value::ofFloat(1.0f));
}
