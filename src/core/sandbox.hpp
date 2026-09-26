#pragma once

#include <cstdint>

/// **The test binaries' sandbox: nothing a test does reaches the rig** (the audit of 2026-09-25,
/// T1 and T2, and the operator's answer to its Q8).
///
/// The machine the tests run on is the one the show runs from. A window test pressed
/// [+ ADD OUTPUT] and sent real OSC to 127.0.0.1:9000, the port this rig's own receiver listens
/// on; a click sweep landed on "listen" and bound the OSC control port out from under a takt4
/// that was running the show — a loopback bind wins every loopback datagram over a `0.0.0.0`
/// holder of the same port (measured). Changing those tests closes those two. This closes the
/// kind, so the next test cannot open it again by accident.
///
/// Switched on by `TAKT4_TEST_SANDBOX`, which the test executables set before `main`
/// (tests/support/crt_dialogs.cpp) and so hand on to anything they start. A test tagged
/// [network] or [hardware] — the ones the `-all` presets exist for — has it switched off for its
/// own run and no longer (tests/support/rig_sandbox.cpp). takt4 itself never sets it, and
/// outside the tests every check here is one relaxed load. While it is on:
///
/// - **A datagram goes only to a port on this machine that a test's own receiver was given**
///   (`allowPort`), or into 0.0.0.0/8, where no system delivers anything — the tests' stand-in
///   for a network that is down, which has to fail the way a real one does. Anything else is
///   dropped here and reported to the sender as sent, so the code under test behaves exactly
///   as it would with the datagram gone; `refusals` counts them.
/// - **A socket binds only to port 0, or to such a port** — never to one a program on the rig
///   may be listening on, takt4's own 7001 above all.
/// - **No MIDI port is opened**, input or output: the machine's ports are the rig's controllers,
///   lighting desk and synths — and Windows' GS Wavetable Synth plays through the speakers.
/// - **No Link session is joined**, and no Link announcements are listened for.
/// - **No audio device is opened.**
/// - **No name is looked up**, apart from `localhost`: a host typed as a name would send a DNS
///   query out of the rig's network card (the audit of 2026-09-25, T4). It fails the way a name
///   nobody knows fails — "cannot resolve" — so the tests of that path still see it.
///
/// Looking a device up is left alone: a port that is not there is still reported as not
/// there, in the words an operator would get. Only the step that would open it is refused,
/// with a message that says why.
namespace takt4::sandbox {

/// Whether the sandbox is on. Read from `TAKT4_TEST_SANDBOX` the first time it is asked, then
/// whatever `setActive` last said.
bool active() noexcept;

/// Switches it, for the rest of the process or until the next call. For the test harness's
/// per-test switch and the sandbox's own tests; nothing in takt4 calls it.
void setActive(bool on) noexcept;

/// A port a test's own receiver on the loopback was given by the system: sends to it are let
/// through, and so is a bind to it — a test that probes for a free port and then asks takt4 to
/// listen there is how the "port in use" paths are tested. Once allowed, allowed for the life of
/// the process: the system handed the number out as free, so nothing on the rig owns it.
void allowPort(std::uint16_t port) noexcept;

/// Whether a datagram may go to this IPv4 address (host byte order) and port.
bool allowsSend(std::uint32_t ipv4, std::uint16_t port) noexcept;

/// Whether a socket may be bound to this port.
bool allowsBind(std::uint16_t port) noexcept;

/// What was refused, for a test that wants to know nothing was.
enum class Refused : std::uint8_t { Nothing, Send, Bind, Midi, Link, Audio, Lookup };

/// Records a refusal. Allocates nothing: a send is refused on the output thread, which the
/// real-time allocation guard is watching in the tests.
void refuse(Refused what, std::uint16_t port = 0) noexcept;

/// How many things have been refused since the process started.
std::uint64_t refusals() noexcept;
/// How many of one kind.
std::uint64_t refusals(Refused what) noexcept;

/// The last thing refused, and its port where it had one.
Refused lastRefused() noexcept;
std::uint16_t lastRefusedPort() noexcept;

} // namespace takt4::sandbox
