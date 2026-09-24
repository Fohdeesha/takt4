#include "core/control/osc_parse.hpp"
#include "core/output/osc_message.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::WithinAbs;
using takt4::control::OscArgument;
using takt4::control::OscView;
using takt4::control::parseOsc;
using takt4::output::OscMessage;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

/// A packet written out by hand, with '.' standing for a null so the padding is visible.
std::vector<std::byte> packet(std::string_view sketch) {
    std::string text(sketch);
    for (char& c : text) {
        if (c == '.') {
            c = '\0';
        }
    }
    return bytes(text);
}

} // namespace

TEST_CASE("what the sender builds, the receiver reads", "[control]") {
    // The strongest thing that can be said about two halves of one format: they are held
    // to each other rather than to this test's idea of the spec.
    OscMessage message("/takt4/ctl/tempo");
    message.addInt(128).addFloat(0.75f).addString("locked");
    const std::span<const std::byte> encoded = message.packet();
    REQUIRE(message.valid());

    const auto view = parseOsc(encoded);
    REQUIRE(view.has_value());
    CHECK(view->address() == "/takt4/ctl/tempo");
    REQUIRE(view->argumentCount() == 3);

    CHECK(view->argument(0).type == OscArgument::Type::Int32);
    CHECK(view->argument(0).asInt == 128);
    CHECK(view->argument(1).type == OscArgument::Type::Float32);
    CHECK_THAT(view->argument(1).asFloat, WithinAbs(0.75, 1e-9));
    CHECK(view->argument(2).type == OscArgument::Type::String);
    CHECK(view->argument(2).asString == "locked");

    CHECK_THAT(*view->number(0), WithinAbs(128.0, 1e-9));
    CHECK_THAT(*view->number(1), WithinAbs(0.75, 1e-9));
    CHECK_FALSE(view->number(2).has_value()); // a string is not a number
    CHECK_FALSE(view->number(9).has_value());
}

TEST_CASE("an address on its own is a message", "[control]") {
    // `/ctl/tap` carries nothing, and enough senders omit the type tag string entirely
    // that refusing them would mean refusing real control surfaces.
    const auto bare = packet("/ctl/tap....");
    const auto view = parseOsc(bare);
    REQUIRE(view.has_value());
    CHECK(view->address() == "/ctl/tap");
    CHECK(view->argumentCount() == 0);
    CHECK_FALSE(view->number(0).has_value()); // "the sender did not say", not "zero"

    // And with an empty tag string, which is the same message spelled properly.
    OscMessage message("/ctl/tap");
    const auto proper = parseOsc(message.packet());
    REQUIRE(proper.has_value());
    CHECK(proper->address() == "/ctl/tap");
    CHECK(proper->argumentCount() == 0);
}

TEST_CASE("a flag argument reads the way a control surface sends it", "[control]") {
    // §5.7's `<0|1>`. A Stream Deck sending a boolean means what one sending 1 or 0 means, so
    // `T` and `F` read as the numbers they stand for.
    OscMessage one("/ctl/lock");
    one.addInt(1);
    CHECK(parseOsc(one.packet())->number(0) == 1.0);

    OscMessage zero("/ctl/lock");
    zero.addInt(0);
    CHECK(parseOsc(zero.packet())->number(0) == 0.0);

    OscMessage half("/ctl/lock");
    half.addFloat(0.5f);
    CHECK(parseOsc(half.packet())->number(0) == 0.5);

    const auto boolean = packet("/ctl/lock...,TF.");
    const auto view = parseOsc(boolean);
    REQUIRE(view.has_value());
    REQUIRE(view->argumentCount() == 2);
    CHECK(view->number(0) == 1.0);
    CHECK(view->number(1) == 0.0);
}

TEST_CASE("a datagram that is not a message is refused, not guessed at", "[control]") {
    // Everything here came off a socket, so the only safe answer to anything surprising
    // is nothing at all.
    CHECK_FALSE(parseOsc({}).has_value());
    CHECK_FALSE(parseOsc(packet("/ctl")).has_value());             // no terminator
    CHECK_FALSE(parseOsc(packet("/ctl/tap.")).has_value());        // not padded to four
    CHECK_FALSE(parseOsc(packet("ctl/tap.")).has_value());         // no leading slash
    CHECK_FALSE(parseOsc(packet("........")).has_value());         // empty address
    CHECK_FALSE(parseOsc(packet("#bundle.")).has_value());         // a bundle, not a message
    CHECK_FALSE(parseOsc(packet("/ctl/tap....xi..")).has_value()); // tag string without ','

    // A tag whose width this does not know: the arguments after it cannot be found, so
    // acting on any of them would mean acting on numbers nobody sent.
    CHECK_FALSE(parseOsc(packet("/ctl/tap....,h..")).has_value());
    CHECK_FALSE(parseOsc(packet("/ctl/tap....,b..")).has_value());

    // An argument the packet does not actually contain.
    CHECK_FALSE(parseOsc(packet("/ctl/tap....,i..")).has_value());
    CHECK_FALSE(parseOsc(packet("/ctl/tap....,ii.1234")).has_value());
    // A string argument with no terminator.
    CHECK_FALSE(parseOsc(packet("/ctl/tap....,s..abcd")).has_value());
    // An address with bytes outside printable ASCII.
    CHECK_FALSE(parseOsc(bytes(std::string("/ct\x01") + std::string(4, '\0'))).has_value());
}

TEST_CASE("more arguments than the format holds are refused, not truncated", "[control]") {
    // Silently dropping arguments is how a control surface ends up doing something other
    // than what it said.
    std::string tags = ",";
    tags.append(OscView::kMaxArguments + 1, 'T');
    std::string text = "/ctl/x";
    text.resize(8, '\0'); // address padded
    text += tags;
    text.resize(((text.size() / 4) + 1) * 4, '\0');
    CHECK_FALSE(parseOsc(bytes(text)).has_value());

    // Exactly the maximum is fine.
    std::string ok = "/ctl/x";
    ok.resize(8, '\0');
    ok += "," + std::string(OscView::kMaxArguments, 'T');
    ok.resize(((ok.size() / 4) + 1) * 4, '\0');
    const auto view = parseOsc(bytes(ok));
    REQUIRE(view.has_value());
    CHECK(view->argumentCount() == OscView::kMaxArguments);
}

TEST_CASE("truncating a real message at any length never reads past the end", "[control]") {
    // A datagram can arrive cut short, and one deliberately cut short is the first thing
    // anyone points at a listening socket. Every prefix must be refused or parsed, never
    // crash — run this under the allocation guard and a sanitizer alike.
    OscMessage message("/takt4/ctl/tempo/halve");
    message.addInt(-1).addFloat(128.5f).addString("a longer string argument");
    const std::span<const std::byte> full = message.packet();
    const std::vector<std::byte> whole(full.begin(), full.end());
    REQUIRE(whole.size() > 16);

    // **Each prefix in an allocation of exactly its own length.** Sliced out of `whole`, a
    // read one byte past the end of a prefix is still inside `whole`, so no sanitizer can see
    // it — which made the "never reads past the end" of this test's name a claim nothing
    // checked (the audit's T1). A heap block of `length` bytes puts ASan's redzone straight
    // after the last one.
    std::size_t parsed = 0;
    for (std::size_t length = 0; length < whole.size(); ++length) {
        const std::unique_ptr<std::byte[]> prefix(new std::byte[length == 0 ? 1 : length]);
        std::copy_n(whole.begin(), length, prefix.get());
        const auto view = parseOsc(std::span<const std::byte>(prefix.get(), length));
        if (view) {
            ++parsed;
            // Anything that did parse has to be self-consistent.
            CHECK(view->address().front() == '/');
            CHECK(view->argumentCount() <= OscView::kMaxArguments);
        }
    }
    INFO(parsed << " of " << whole.size() << " truncations parsed as something");

    // And every single-byte corruption is either refused or parsed, never worse.
    for (std::size_t i = 0; i < whole.size(); ++i) {
        for (const std::byte flip : {std::byte{0x00}, std::byte{0xFF}, std::byte{0x2C}}) {
            std::vector<std::byte> damaged = whole;
            damaged[i] = flip;
            const auto view = parseOsc(damaged);
            if (view) {
                CHECK(view->address().size() < damaged.size());
                CHECK(view->argumentCount() <= OscView::kMaxArguments);
            }
        }
    }
}
