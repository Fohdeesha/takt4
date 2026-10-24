#include "core/io/utf8.hpp"
#include "core/timing/prolink/network.hpp"
#include "core/timing/prolink/packets.hpp"

#include "support/prolink_capture.hpp"
#include "support/prolink_peer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using takt4::testing::ipv4;
using namespace takt4::timing::prolink;
using namespace std::chrono_literals;

// The network card the CDJs are joined on: which of this machine's cards a card picked is, which
// cards could reach a CDJ at all, and a card's name made fit to show and to save — whatever the
// system allows in one.

namespace {

Interface card(std::string name, std::uint32_t ip, std::uint32_t mask, Mac mac,
               std::uint32_t index) {
    Interface made;
    made.name = std::move(name);
    made.ip = ip;
    made.mask = mask;
    made.broadcast = ip | ~mask;
    made.mac = mac;
    made.index = index;
    return made;
}

constexpr Mac kCable{0x00, 0x1b, 0x21, 0x3a, 0x4c, 0x5d};
constexpr Mac kWifi{0x3c, 0x22, 0xfb, 0x01, 0x02, 0x03};
constexpr Mac kElsewhere{0x00, 0x0e, 0xc6, 0x11, 0x22, 0x33};

/// The code points of `text`, which is well-formed UTF-8.
std::vector<char32_t> codePoints(const std::string& text) {
    std::vector<char32_t> out;
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        char32_t c = length == 1   ? lead
                     : length == 2 ? lead & 0x1f
                     : length == 3 ? lead & 0x0f
                                   : lead & 0x07;
        for (std::size_t i = 1; i < length; ++i) {
            c = (c << 6) | (static_cast<unsigned char>(text[at + i]) & 0x3f);
        }
        out.push_back(c);
        at += length;
    }
    return out;
}

/// Anything a card's name must not show: a control character or line break, a mark that reorders
/// the text, a zero-width space or byte order mark.
bool showsNothingItShouldNot(const std::string& text) {
    for (const char32_t c : codePoints(text)) {
        const bool control = c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0x2028 || c == 0x2029;
        const bool hidden = c == 0x061c || c == 0x200b || c == 0x200e || c == 0x200f ||
                            (c >= 0x202a && c <= 0x202e) || c == 0x2060 ||
                            (c >= 0x2066 && c <= 0x2069) || c == 0xfeff;
        if (control || hidden) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("a card picked is found by its address and hardware address, by its address alone, and "
          "on a new address by its hardware address, never by its name",
          "[prolink]") {
    const Interface wifi = card("Wi-Fi", ipv4(192, 168, 1, 23), 0xffffff00u, kWifi, 7);
    const Interface cable = card("Ethernet 2", ipv4(169, 254, 20, 7), 0xffff0000u, kCable, 12);
    const std::vector<Interface> cards{wifi, cable};

    CHECK(pickedCard(cards, {cable.ip, kCable, "Ethernet 2"})->ip == cable.ip);
    // Renamed since, or picked under another card's name: the name finds nothing.
    CHECK(pickedCard(cards, {cable.ip, kCable, "Wi-Fi"})->ip == cable.ip);
    // By address alone — the console's --interface, a settings file from before the hardware
    // address was kept.
    CHECK(pickedCard(cards, {cable.ip, Mac{}, ""})->ip == cable.ip);

    // A new link-local address, or a lease changed: the same card, by its hardware address.
    std::vector<Interface> moved = cards;
    moved[1].ip = ipv4(169, 254, 99, 3);
    const std::optional<Interface> again = pickedCard(moved, {cable.ip, kCable, "Ethernet 2"});
    REQUIRE(again.has_value());
    CHECK(again->ip == ipv4(169, 254, 99, 3));
    CHECK(again->index == 12);
    // By address alone and the address gone: nothing is guessed.
    CHECK_FALSE(pickedCard(moved, {cable.ip, Mac{}, ""}).has_value());

    // Another computer's card — the settings file beside a takt4 on a shared drive: nothing.
    CHECK_FALSE(pickedCard(cards, {ipv4(10, 9, 9, 9), kElsewhere, "DJ LAN"}).has_value());
    CHECK_FALSE(pickedCard({}, {cable.ip, kCable, "Ethernet 2"}).has_value());

    // The address on one card and the hardware address on another: the address, which is where a
    // device on that network is answered from.
    const Interface usb = card("USB LAN", cable.ip, 0xffff0000u, kElsewhere, 15);
    const Interface renumbered = card("Ethernet 2", ipv4(169, 254, 1, 1), 0xffff0000u, kCable, 12);
    CHECK(pickedCard({renumbered, usb}, {cable.ip, kCable, "Ethernet 2"})->index == 15);
    // An address of 0 is no address: found by its hardware address, if at all.
    CHECK(pickedCard(cards, {0, kCable, ""})->index == 12);
    CHECK_FALSE(pickedCard(cards, {0, Mac{}, ""}).has_value());
}

TEST_CASE("cards that share a hardware address are told apart by the address picked, and none is "
          "guessed when it has gone; one card on two addresses is one card",
          "[prolink]") {
    // Hyper-V's external switch carries the hardware address of the card it is on, and so does a
    // Linux bridge or a VLAN: two cards, one hardware address.
    const Interface vswitch =
        card("vEthernet (External)", ipv4(192, 168, 1, 30), 0xffffff00u, kCable, 21);
    const Interface physical = card("Ethernet", ipv4(169, 254, 5, 5), 0xffff0000u, kCable, 12);
    const std::vector<Interface> shared{vswitch, physical};
    CHECK(pickedCard(shared, {physical.ip, kCable, ""})->index == 12);
    CHECK(pickedCard(shared, {vswitch.ip, kCable, ""})->index == 21);
    CHECK_FALSE(pickedCard(shared, {ipv4(169, 254, 9, 9), kCable, ""}).has_value());

    // One card listed once for each of two addresses, its old one gone: found.
    const Interface second = card("Ethernet", ipv4(10, 0, 0, 5), 0xff000000u, kCable, 12);
    CHECK(pickedCard({physical, second}, {ipv4(169, 254, 9, 9), kCable, ""})->index == 12);
    // With no number from the system, the same card by the system's name for it — and two names
    // are two cards.
    Interface a = physical;
    Interface b = second;
    a.index = 0;
    b.index = 0;
    CHECK(pickedCard({a, b}, {ipv4(169, 254, 9, 9), kCable, ""}).has_value());
    b.name = "eth1";
    CHECK_FALSE(pickedCard({a, b}, {ipv4(169, 254, 9, 9), kCable, ""}).has_value());
}

TEST_CASE("only a card that could reach a CDJ is one to pick: not the loopback, not a network of "
          "one address, not a card with no address or no network",
          "[prolink]") {
    CHECK(reachesDevices(card("Ethernet 2", ipv4(169, 254, 20, 7), 0xffff0000u, kCable, 12)));
    CHECK(reachesDevices(card("Wi-Fi", ipv4(192, 168, 1, 23), 0xffffff00u, kWifi, 7)));
    Interface loopback =
        card("Loopback Pseudo-Interface 1", ipv4(127, 0, 0, 1), 0xff000000u, Mac{}, 1);
    loopback.loopback = true;
    CHECK_FALSE(reachesDevices(loopback));
    // A VPN's: itself alone.
    CHECK_FALSE(reachesDevices(card("WireGuard", ipv4(10, 64, 0, 2), 0xffffffffu, Mac{}, 30)));
    CHECK_FALSE(reachesDevices(card("no network", ipv4(10, 0, 0, 1), 0u, Mac{}, 31)));
    CHECK_FALSE(reachesDevices(card("no address", 0u, 0xffffff00u, Mac{}, 32)));
}

TEST_CASE("this machine's cards have names fit to show, and its loopback is marked as the system "
          "marks it",
          "[prolink]") {
    const std::vector<Interface> cards = listInterfaces();
    bool loopbackListed = false;
    for (const Interface& listed : cards) {
        INFO(cardText(listed));
        CHECK(takt4::io::isValidUtf8(listed.name));
        CHECK(cardName(listed.name) == listed.name);
        if ((listed.ip >> 24) == 127) {
            CHECK(listed.loopback);
            loopbackListed = true;
        } else {
            CHECK_FALSE(listed.loopback);
        }
    }
    CHECK(loopbackListed);
}

TEST_CASE("a card's name is made fit to show and to save, whatever the system allows in one",
          "[prolink]") {
    CHECK(cardName("Ethernet 2") == "Ethernet 2");
    // Bytes that are not UTF-8, as a Linux interface can be renamed to: U+FFFD each.
    CHECK(cardName(std::string("eth\xff\xfe"
                               "0")) == "eth\xEF\xBF\xBD\xEF\xBF\xBD"
                                        "0");
    // Line breaks, tabs and escapes: spaces, one for a run, none at either end.
    CHECK(cardName("\tDJ\r\nLAN \x1b[31m 2\n") == "DJ LAN [31m 2");
    CHECK(cardName("a\xC2\x85"
                   "b\xE2\x80\xA8"
                   "c\x7f"
                   "d") == "a b c d");
    // The marks that reorder text, out: an override would show the address after it backwards.
    CHECK(cardName("\xE2\x80\xAE"
                   "NAL JD") == "NAL JD");
    CHECK(cardName("DJ\xE2\x81\xA6 LAN\xE2\x81\xA9\xE2\x80\x8F") == "DJ LAN");
    CHECK(cardName("\xD8\x9C"
                   "DJ\xE2\x80\x8E") == "DJ");
    // Zero-width spaces, a word joiner, a byte order mark: out.
    CHECK(cardName("\xEF\xBB\xBF"
                   "D\xE2\x80\x8BJ\xE2\x81\xA0!") == "DJ!");
    // What is meant to be there stays: emoji with their joiners, other scripts, right-to-left
    // text written as such, combining marks, quotes and backslashes.
    for (const std::string& kept :
         {std::string("\xF0\x9F\x8E\xA7 DJ"),
          std::string("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"),
          std::string("\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0"
                      "\xB8\xD0\xB5 2"),
          std::string("\xE3\x82\xA4\xE3\x83\xBC\xE3\x82\xB5\xE3\x83\x8D\xE3\x83\x83\xE3\x83\x88"),
          std::string("\xD7\xA9\xD7\xA7\xD7\xA2 \xD7\xA8\xD7\xA9\xD7\xAA"),
          std::string("Cafe\xCC\x81"), std::string("\"Desk\" \\ LAN's")}) {
        CHECK(cardName(kept) == kept);
    }
    // Nothing left: empty, and the card is shown by its address alone.
    CHECK(cardName(" \t\xE2\x80\xAE\n ").empty());
    CHECK(cardName("").empty());
    // Long: cut to `kCardNameLength` characters, the last an ellipsis — counted in characters,
    // never through one, and no space left before the ellipsis. Windows' longest name of its own
    // is kept whole.
    constexpr std::size_t n = kCardNameLength;
    CHECK(cardName("Bluetooth Network Connection") == "Bluetooth Network Connection");
    CHECK(cardName(std::string(n, 'y')) == std::string(n, 'y'));
    CHECK(cardName(std::string(n + 1, 'y')) == std::string(n - 1, 'y') + "\xE2\x80\xA6");
    CHECK(cardName(std::string(300, 'x')) == std::string(n - 1, 'x') + "\xE2\x80\xA6");
    std::string euros;
    for (int i = 0; i < 100; ++i) {
        euros += "\xE2\x82\xAC";
    }
    const std::string cutEuros = cardName(euros);
    CHECK(takt4::io::isValidUtf8(cutEuros));
    CHECK(codePoints(cutEuros).size() == n);
    CHECK(cardName(std::string(n - 2, 'z') + " " + std::string(10, 'z')) ==
          std::string(n - 2, 'z') + "\xE2\x80\xA6");
}

TEST_CASE("any bytes at all make a name that is UTF-8, shows nothing it should not, is no longer "
          "than a name may be, and comes out the same made fit twice",
          "[prolink]") {
    // A fixed generator, so a failure is the same failure every run.
    std::uint64_t state = 0x9e3779b97f4a7c15u;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    // Bytes from everywhere, weighted towards the ones that make trouble: lead and continuation
    // bytes, controls, and whole sequences of the marks that are taken out.
    const std::vector<std::string> pieces{"\xE2\x80\xAE",
                                          "\xE2\x81\xA6",
                                          "\xE2\x80\x8B",
                                          "\xEF\xBB\xBF",
                                          "\xC2\x85",
                                          "\xE2\x80\xA8",
                                          "\xF0\x9F\x8E\xA7",
                                          " ",
                                          "\n"};
    for (int round = 0; round < 3000; ++round) {
        std::string raw;
        const std::size_t length = next() % 160;
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint64_t r = next();
            if (r % 5 == 0) {
                raw += pieces[(r >> 8) % pieces.size()];
            } else {
                raw.push_back(static_cast<char>((r >> 16) & 0xff));
            }
        }
        const std::string name = cardName(raw);
        INFO("round " << round);
        REQUIRE(takt4::io::isValidUtf8(name));
        CHECK(showsNothingItShouldNot(name));
        CHECK(codePoints(name).size() <= kCardNameLength);
        CHECK(cardName(name) == name);
        CHECK((name.empty() || (name.front() != ' ' && name.back() != ' ')));
        CHECK(name.find("  ") == std::string::npos);
    }
}

TEST_CASE("a card is shown as its name and address, or its address alone, made fit to show "
          "wherever its name came from",
          "[prolink]") {
    CHECK(cardText(card("Ethernet 2", ipv4(169, 254, 20, 7), 0xffff0000u, kCable, 12)) ==
          "Ethernet 2 \xC2\xB7 169.254.20.7");
    CHECK(cardText(card("", ipv4(169, 254, 20, 7), 0xffff0000u, kCable, 12)) == "169.254.20.7");
    // A test's own cards, or any that did not come through the system's list: made fit here too.
    CHECK(cardText(card("\xff\nLAN", ipv4(10, 0, 0, 5), 0xff000000u, kCable, 12)) ==
          "\xEF\xBF\xBD LAN \xC2\xB7 10.0.0.5");
    CHECK(pickText({ipv4(10, 9, 9, 9), Mac{}, ""}) == "10.9.9.9");
    CHECK(pickText({ipv4(10, 9, 9, 9), kElsewhere, "DJ\nLAN"}) == "DJ LAN \xC2\xB7 10.9.9.9");
}

TEST_CASE("a hardware address is kept as text and read back, and anything else is not one",
          "[prolink]") {
    const Mac mac{0x74, 0x5e, 0x1c, 0x00, 0x00, 0x02};
    CHECK(macText(mac) == "74:5e:1c:00:00:02");
    CHECK(macText(Mac{0xff, 0xee, 0x0a, 0xb0, 0x01, 0x10}) == "ff:ee:0a:b0:01:10");
    CHECK(parseMac("74:5e:1c:00:00:02") == mac);
    CHECK(parseMac("74-5E-1C-00-00-02") == mac);
    CHECK(parseMac(macText(kCable)) == kCable);
    for (const char* bad : {"", "74:5e:1c:00:00", "74:5e:1c:00:00:0g", "745e1c000002",
                            "74:5e:1c:00:00:02:", "74 5e 1c 00 00 02", "74:5e:1c:00:00:2",
                            ":74:5e:1c:00:00:0", "74:5e:1c:00:00:02\n"}) {
        INFO(bad);
        CHECK_FALSE(parseMac(bad).has_value());
    }
}

#if defined(_WIN32)
TEST_CASE("a Windows name with an unpaired surrogate comes out as UTF-8, the half a U+FFFD",
          "[prolink]") {
    CHECK(wideName(nullptr).empty());
    CHECK(wideName(L"").empty());
    CHECK(wideName(L"Ethernet 2") == "Ethernet 2");
    const wchar_t pair[] = {L'D', 0xD83C, 0xDFA7, L'J', 0};
    CHECK(wideName(pair) == "D\xF0\x9F\x8E\xA7J");
    const wchar_t high[] = {L'D', 0xD83C, L'J', 0};
    CHECK(wideName(high) == "D\xEF\xBF\xBDJ");
    const wchar_t low[] = {0xDFA7, L'J', 0};
    CHECK(wideName(low) == "\xEF\xBF\xBDJ");
    const wchar_t trailing[] = {L'J', 0xD83C, 0};
    CHECK(wideName(trailing) == "J\xEF\xBF\xBD");
    for (const wchar_t* name : {pair, high, low, trailing}) {
        CHECK(takt4::io::isValidUtf8(wideName(name)));
    }
}
#endif

TEST_CASE("on sockets, the card picked is the one joined on, though the CDJs are heard on another; "
          "on a new address it is found by its hardware address; and gone, they are joined where "
          "they are heard, saying so",
          "[prolink][slow]") {
    namespace peer = takt4::testing::prolink_peer;
    if (!peer::canUseSecondLoopbackAddress() || !peer::canUseThirdLoopbackAddress()) {
        SKIP("this system has no 127.0.0.2 or 127.0.0.3");
    }
    // The loopback the devices are heard on, listed first, and a second card of the machine's on
    // the same network.
    const Mac secondMac{0x02, 0x00, 0x00, 0x00, 0x00, 0x03};
    const auto cards = [secondMac] {
        std::vector<Interface> listed = peer::loopbackCard();
        Interface second = listed[0];
        second.name = "second";
        second.ip = peer::kMoved;
        second.mac = secondMac;
        listed.push_back(second);
        return listed;
    };
    peer::SimulatedRig rig;
    Network::Config config = peer::testConfig(rig.announcePort(), peer::steadyMicros);
    config.interfaces = cards;
    std::string expectedNote;
    std::uint32_t expectedIp = 0;

    SECTION("picked by both its addresses") {
        config.interfacePick = CardPick{peer::kMoved, secondMac, "second"};
        expectedIp = peer::kMoved;
    }
    SECTION("picked on an address it has not got any more: found by its hardware address") {
        config.interfacePick = CardPick{ipv4(127, 0, 0, 9), secondMac, "second"};
        expectedIp = peer::kMoved;
    }
    SECTION("another computer's: joined where the devices are heard, and said") {
        config.interfacePick = CardPick{ipv4(10, 9, 9, 9), kElsewhere, "DJ LAN"};
        expectedIp = peer::kHere;
        expectedNote = "the card picked for the CDJs, DJ LAN \xC2\xB7 10.9.9.9, is not on this "
                       "computer \xE2\x80\x94 joined on loopback \xC2\xB7 127.0.0.1";
    }
    Network network(config);
    std::string problem;
    REQUIRE(network.open(problem));
    rig.start(config);
    REQUIRE(peer::eventually([&] { return network.snapshot().devices.size() == 2; }, 3s));
    // What the window names before START: the first device heard.
    CHECK(network.snapshot().heardIp == peer::kDevices);
    CHECK(network.cards().size() == 2);
    network.join();
    REQUIRE(peer::eventually(
        [&] { return network.snapshot().phase == VirtualPlayer::Phase::Joined; }, 4s));
    CHECK(network.snapshot().interfaceIp == expectedIp);
    CHECK(network.snapshot().note == expectedNote);
    CHECK(network.snapshot().problem.empty());
    rig.stop();
}
