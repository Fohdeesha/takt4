#include "core/remote/status_messages.hpp"

#include "core/output/osc_message.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace takt4::remote {
namespace {

/// `text` cut to at most `most` bytes, on a character's boundary — what an operator typed can be
/// any length, and a message has `OscMessage::kCapacity` bytes for its address and all of its
/// arguments; one that would not fit is not sent at all.
std::string_view clipped(std::string_view text, std::size_t most) {
    if (text.size() <= most) {
        return text;
    }
    std::size_t end = most;
    // Back off a UTF-8 continuation byte, so a name is never cut inside a character.
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) {
        --end;
    }
    return text.substr(0, end);
}

std::int32_t clampedInt(std::uint64_t value) {
    return static_cast<std::int32_t>(std::min<std::uint64_t>(
        value, static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
}

std::int32_t clampedInt(std::int64_t value) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        value, 0, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

/// One message, `prefix` + `tail`, filled by `fill`, appended when it came out whole.
template <typename Fill>
void add(std::vector<Packet>& out, std::string_view prefix, std::string_view tail, Fill&& fill) {
    const std::string address = std::string(prefix) + std::string(tail);
    output::OscMessage message(address);
    fill(message);
    if (!message.valid()) {
        return;
    }
    const std::span<const std::byte> bytes = message.packet();
    if (!bytes.empty()) {
        out.emplace_back(bytes.begin(), bytes.end());
    }
}

/// Moves smaller than these are not a change worth a datagram of its own: the next snapshot
/// carries them. The tempo's matches the global `/bpm`'s.
constexpr double kBpmStep = 0.01;
constexpr double kConfidenceStep = 0.05;
constexpr double kLatencyStep = 0.1;

/// The values of `status`, every one when `before` is null, otherwise those that moved from it.
/// **The input level is never sent on a change**: it moves with every hop of music, and would be a
/// datagram every redraw; the snapshot carries it once a second.
void values(std::vector<Packet>& out, std::string_view prefix, const Status& s,
            const Status* before) {
    const bool all = before == nullptr;
    const auto flag = [&](std::string_view tail, bool now, bool was) {
        if (all || now != was) {
            add(out, prefix, tail, [&](output::OscMessage& m) { m.addInt(now ? 1 : 0); });
        }
    };
    const auto number = [&](std::string_view tail, double now, double was, double step) {
        if (all || std::abs(now - was) >= step) {
            add(out, prefix, tail,
                [&](output::OscMessage& m) { m.addFloat(static_cast<float>(now)); });
        }
    };
    const auto integer = [&](std::string_view tail, std::int32_t now, std::int32_t was) {
        if (all || now != was) {
            add(out, prefix, tail, [&](output::OscMessage& m) { m.addInt(now); });
        }
    };
    const auto text = [&](std::string_view tail, const std::string& now, const std::string& was,
                          std::size_t most) {
        if (all || now != was) {
            add(out, prefix, tail, [&](output::OscMessage& m) { m.addString(clipped(now, most)); });
        }
    };
    const Status none;
    const Status& b = all ? none : *before;
    flag("/status/running", s.running, b.running);
    flag("/status/locked", s.locked, b.locked);
    flag("/status/hunting", s.hunting, b.hunting);
    text("/status/state", s.state, b.state, 32);
    number("/status/confidence", s.confidence, b.confidence, kConfidenceStep);
    number("/status/bpm", s.bpm, b.bpm, kBpmStep);
    if (all) {
        add(out, prefix, "/status/level",
            [&](output::OscMessage& m) { m.addFloat(static_cast<float>(s.levelDb)); });
        // As the level: the worst of each second, sent with the second.
        add(out, prefix, "/status/load",
            [&](output::OscMessage& m) { m.addFloat(static_cast<float>(s.load)); });
    }
    integer("/status/dropouts", clampedInt(s.dropouts), clampedInt(b.dropouts));
    text("/status/source", s.source, b.source, 32);
    // The detail before source_ok, and again with every change of it: a show controller that
    // judges a change of source_ok at the end of each datagram has the reason by then.
    if (all || s.sourceDetail != b.sourceDetail || s.sourceOk != b.sourceOk) {
        add(out, prefix, "/status/source_detail",
            [&](output::OscMessage& m) { m.addString(clipped(s.sourceDetail, 160)); });
    }
    flag("/status/source_ok", s.sourceOk, b.sourceOk);
    flag("/status/panic", s.panic, b.panic);
    // The preset before whether the show is still it, and again with every change of that, as the
    // source's detail comes before source_ok: a controller that sees preset_modified go to 0 has
    // the name of the preset just loaded by then.
    if (all || s.loadedPreset != b.loadedPreset || s.presetModified != b.presetModified) {
        add(out, prefix, "/status/loaded_preset",
            [&](output::OscMessage& m) { m.addString(clipped(s.loadedPreset, 64)); });
    }
    flag("/status/preset_modified", s.presetModified, b.presetModified);
    text("/status/audio/host", s.audioHost, b.audioHost, 64);
    text("/status/audio/device", s.audioDevice, b.audioDevice, 160);
    integer("/status/audio/channel", s.audioChannel, b.audioChannel);
    flag("/status/audio/mono", s.audioMono, b.audioMono);
    integer("/status/audio/rate", s.audioRate, b.audioRate);
    number("/status/latency_ms", s.latencyMs, b.latencyMs, kLatencyStep);
    text("/status/version", s.version, b.version, 64);
}

} // namespace

std::vector<Packet> statusMessages(std::string_view prefix, const Status& status,
                                   std::int64_t uptimeSeconds, std::int64_t uiAgeMs) {
    std::vector<Packet> out;
    values(out, prefix, status, nullptr);
    add(out, prefix, "/status/uptime_s",
        [&](output::OscMessage& m) { m.addInt(clampedInt(uptimeSeconds)); });
    add(out, prefix, "/status/ui_age_ms",
        [&](output::OscMessage& m) { m.addInt(clampedInt(uiAgeMs)); });
    return out;
}

std::vector<Packet> changedMessages(std::string_view prefix, const Status& before,
                                    const Status& after) {
    std::vector<Packet> out;
    values(out, prefix, after, &before);
    return out;
}

Packet aliveMessage(std::string_view prefix, std::int32_t count) {
    std::vector<Packet> out;
    add(out, prefix, "/status/alive", [&](output::OscMessage& m) { m.addInt(count); });
    return out.empty() ? Packet{} : std::move(out.front());
}

std::vector<Packet> deviceMessages(std::string_view prefix, const Status& status) {
    std::vector<Packet> out;
    for (const DeviceRow& device : status.devices) {
        add(out, prefix, "/status/audio/device", [&](output::OscMessage& m) {
            m.addString(clipped(device.host, 48));
            m.addString(clipped(device.device, 120));
            m.addInt(device.inputs);
        });
    }
    add(out, prefix, "/status/audio/devices",
        [&](output::OscMessage& m) { m.addInt(static_cast<std::int32_t>(status.devices.size())); });
    return out;
}

std::vector<Packet> outputMessages(std::string_view prefix, const Status& status) {
    std::vector<Packet> out;
    for (const OutputRow& row : status.outputs) {
        add(out, prefix, "/status/output", [&](output::OscMessage& m) {
            m.addString(clipped(row.id, 16));
            m.addString(clipped(row.name, 48));
            m.addString(clipped(row.kind, 12));
            m.addString(clipped(row.host, 48));
            m.addInt(row.port);
            m.addInt(row.enabled ? 1 : 0);
            m.addFloat(static_cast<float>(row.delayMs));
            m.addInt(row.global ? 1 : 0);
            m.addString(clipped(row.problem, 160));
        });
    }
    add(out, prefix, "/status/outputs",
        [&](output::OscMessage& m) { m.addInt(static_cast<std::int32_t>(status.outputs.size())); });
    return out;
}

std::vector<Packet> presetMessages(std::string_view prefix, const std::vector<std::string>& names) {
    std::vector<Packet> out;
    for (const std::string& name : names) {
        add(out, prefix, "/status/preset", [&](output::OscMessage& m) { m.addString(name); });
    }
    add(out, prefix, "/status/presets",
        [&](output::OscMessage& m) { m.addInt(static_cast<std::int32_t>(names.size())); });
    return out;
}

Packet replyMessage(std::string_view prefix, std::int32_t seq, bool ok, std::string_view note) {
    std::vector<Packet> out;
    add(out, prefix, "/reply", [&](output::OscMessage& m) {
        m.addInt(seq);
        m.addString(ok ? "ok" : "refused");
        m.addString(clipped(note, 160));
    });
    return out.empty() ? Packet{} : std::move(out.front());
}

Packet refusedMessage(std::string_view prefix, std::string_view address, std::string_view why) {
    std::vector<Packet> out;
    add(out, prefix, "/status/refused", [&](output::OscMessage& m) {
        m.addString(clipped(address, 64));
        m.addString(clipped(why, 120));
    });
    return out.empty() ? Packet{} : std::move(out.front());
}

Packet logMessage(std::string_view prefix, const LogLine& line) {
    std::vector<Packet> out;
    add(out, prefix, "/status/log", [&](output::OscMessage& m) {
        m.addString(clipped(line.level, 8));
        m.addString(clipped(line.text, 180));
    });
    return out.empty() ? Packet{} : std::move(out.front());
}

Packet bundleOf(const std::vector<Packet>& messages) {
    Packet out;
    const char head[8] = {'#', 'b', 'u', 'n', 'd', 'l', 'e', '\0'};
    for (const char c : head) {
        out.push_back(static_cast<std::byte>(c));
    }
    // Time tag 1: "immediately", in OSC 1.0's own words.
    for (int i = 0; i < 7; ++i) {
        out.push_back(std::byte{0});
    }
    out.push_back(std::byte{1});
    for (const Packet& message : messages) {
        const auto size = static_cast<std::uint32_t>(message.size());
        out.push_back(static_cast<std::byte>((size >> 24) & 0xFFu));
        out.push_back(static_cast<std::byte>((size >> 16) & 0xFFu));
        out.push_back(static_cast<std::byte>((size >> 8) & 0xFFu));
        out.push_back(static_cast<std::byte>(size & 0xFFu));
        out.insert(out.end(), message.begin(), message.end());
    }
    return out;
}

} // namespace takt4::remote
