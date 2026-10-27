#pragma once

// The OSC that `remote::Status` is said in: one message per value under `<prefix>/status/`, the
// snapshot as one bundle, and the device and output lists a query is answered with.

#include "core/remote/remote.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace takt4::remote {

using Packet = std::vector<std::byte>;

/// Every `/status/*` value of `status`, a message each, in a fixed order — the once-a-second
/// snapshot and the answer to `/ctl/status` alike. `uptimeSeconds` is how long takt4 has run;
/// `uiAgeMs` how long since its window last redrew, which is how a monitor tells "beats fine,
/// window hung" from both fine.
std::vector<Packet> statusMessages(std::string_view prefix, const Status& status,
                                   std::int64_t uptimeSeconds, std::int64_t uiAgeMs);

/// The values that differ between `before` and `after`, a message each: what goes out at once on
/// a change, between the snapshots. The lists, the uptime and the window's age are not among them.
std::vector<Packet> changedMessages(std::string_view prefix, const Status& before,
                                    const Status& after);

/// `/status/alive i count`: the heartbeat.
Packet aliveMessage(std::string_view prefix, std::int32_t count);

/// `/status/audio/device s host s device i inputs` for each device, then
/// `/status/audio/devices i count`.
std::vector<Packet> deviceMessages(std::string_view prefix, const Status& status);

/// `/status/output s id s name s kind s host i port i enabled f delay_ms i global s problem` for
/// each output, then `/status/outputs i count`.
std::vector<Packet> outputMessages(std::string_view prefix, const Status& status);

/// `/status/preset s name` for each show preset, then `/status/presets i count`.
std::vector<Packet> presetMessages(std::string_view prefix, const std::vector<std::string>& names);

/// `/reply i seq s ok|refused s note`, to a command that carried a request number.
Packet replyMessage(std::string_view prefix, std::int32_t seq, bool ok, std::string_view note);

/// `/status/refused s address s why`, to a command without one that was refused.
Packet refusedMessage(std::string_view prefix, std::string_view address, std::string_view why);

/// `/status/log s level s text`.
Packet logMessage(std::string_view prefix, const LogLine& line);

/// An OSC 1.0 bundle holding `messages`, time tag 1 — at once. One datagram, so a snapshot read
/// from it is one moment's, never half of two.
Packet bundleOf(const std::vector<Packet>& messages);

} // namespace takt4::remote
