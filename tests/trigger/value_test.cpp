#include "core/trigger/value.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

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

TEST_CASE("text is cut on a character boundary, never through one", "[trigger][value]") {
    // **A byte-count truncation is not enough, and what it broke was saving the file.**
    // These are written out through nlohmann, whose `dump()` refuses a string that is not
    // valid UTF-8 by throwing — so a text value with a multi-byte character straddling the
    // cut came back with a lead byte and no continuation, and `settings::save` threw
    // `type_error.316` into a Slint callback and into `ui::run`'s save on the way out.
    // Neither can catch it: the process goes and the session's settings go with it.
    //
    // U+00E9 is two bytes, U+20AC three and U+1F3B5 four, so between them they cut at every
    // offset a boundary can fall on.
    const auto valid = [](std::string_view text) {
        // A whole number of well-formed sequences and nothing dangling — the property
        // nlohmann checks, spelled out here rather than inferred from a throw.
        for (std::size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            const std::size_t length = lead < 0x80    ? 1
                                       : lead >= 0xF0 ? 4
                                       : lead >= 0xE0 ? 3
                                       : lead >= 0xC0 ? 2
                                                      : 0; // a continuation byte cannot lead
            if (length == 0 || i + length > text.size()) {
                return false;
            }
            for (std::size_t k = 1; k < length; ++k) {
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                    return false;
                }
            }
            i += length;
        }
        return true;
    };

    for (const std::string_view character : {"\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x8E\xB5"}) {
        // Every offset at which the character can straddle the cut, and a few either side.
        for (std::size_t before = Value::kTextCapacity - 4; before <= Value::kTextCapacity;
             ++before) {
            std::string text(before, 'a');
            text.append(character);
            text.append(8, 'b'); // and enough after it that there is certainly a cut
            const Value value = Value::ofText(text);
            INFO("a " << character.size() << "-byte character after " << before << " bytes");
            CHECK(value.truncated());
            CHECK(value.text().size() <= Value::kTextCapacity);
            CHECK(valid(value.text()));
            // And it gives up at most the one character it could not fit: three bytes for a
            // four-byte character, and nothing at all for an ASCII cut.
            CHECK(value.text().size() + 3 >= Value::kTextCapacity);
        }
    }

    SECTION("text that fits is untouched, whatever is in it") {
        const std::string fits = "drop \xE2\x82\xAC \xF0\x9F\x8E\xB5";
        const Value value = Value::ofText(fits);
        CHECK_FALSE(value.truncated());
        CHECK(value.text() == fits);
    }

    SECTION("and an ASCII cut still fills the buffer") {
        const Value value = Value::ofText(std::string(Value::kTextCapacity + 5, 'x'));
        CHECK(value.truncated());
        CHECK(value.text().size() == Value::kTextCapacity);
    }
}
