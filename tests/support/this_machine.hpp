#pragma once

// Which addresses are this machine's — for the [network] tests that hear Link announcements.
//
// A test that joins Link hears every Link session on the network, not only the ones it made, and
// the rig's network has one of its own (a device at 192.168.1.90 running two sessions, which
// failed two tests on 2026-09-30 by being heard, correctly). A session this process or a peer
// process it starts runs announces from one of this machine's own addresses, so that is what
// such a test counts.

#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
// winsock2.h before windows.h and iphlpapi.h, as they insist.
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#include <array>
#endif

namespace takt4::testing {

/// Every IPv4 address this machine's adapters hold. Empty where it cannot be asked, which a
/// caller takes as "count every peer".
inline std::vector<std::string> thisMachinesAddresses() {
    std::vector<std::string> out;
#if defined(_WIN32)
    ULONG size = 32 * 1024;
    std::vector<unsigned char> buffer(size);
    constexpr ULONG kFlags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = ::GetAdaptersAddresses(
        AF_INET, kFlags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        result = ::GetAdaptersAddresses(AF_INET, kFlags, nullptr,
                                        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                        &size);
    }
    if (result != NO_ERROR) {
        return out;
    }
    for (const IP_ADAPTER_ADDRESSES* adapter =
             reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next) {
            const auto* in = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            std::array<char, INET_ADDRSTRLEN> text{};
            if (::inet_ntop(AF_INET, &in->sin_addr, text.data(), text.size()) != nullptr) {
                out.emplace_back(text.data());
            }
        }
    }
#endif
    return out;
}

/// Whether a peer that announced from `addresses` is on this machine — any of them one of
/// `ours` — or `ours` is empty, when every peer counts.
inline bool onThisMachine(const std::vector<std::string>& addresses,
                          const std::vector<std::string>& ours) {
    if (ours.empty()) {
        return true;
    }
    for (const std::string& address : addresses) {
        for (const std::string& mine : ours) {
            if (address == mine) {
                return true;
            }
        }
    }
    return false;
}

/// The same for a list as the window shows it: "10.0.0.2, 192.168.1.10".
inline bool onThisMachine(std::string_view shown, const std::vector<std::string>& ours) {
    std::vector<std::string> addresses;
    std::size_t at = 0;
    while (at <= shown.size()) {
        const std::size_t comma = shown.find(", ", at);
        const std::size_t end = comma == std::string_view::npos ? shown.size() : comma;
        if (end > at) {
            addresses.emplace_back(shown.substr(at, end - at));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        at = comma + 2;
    }
    return onThisMachine(addresses, ours);
}

} // namespace takt4::testing
