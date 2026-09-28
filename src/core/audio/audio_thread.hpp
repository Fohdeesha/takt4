#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace takt4::audio {

/// An audio driver that did not answer in the time it was given — or one that is still holding
/// the audio thread from an earlier call, so this one was never made. Not a `PortAudioError`: it
/// says nothing about the interface being busy, and the advice a window gives for that ("another
/// application has it") would be the wrong advice.
class DriverNotAnswering : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The one thread every PortAudio call of an `engine::LiveTracker` is made on — and the reason no
/// audio driver can hold the window's thread for longer than the window is prepared to wait.
///
/// **Why a thread of its own** (the audit of 2026-09-25, L23). PortAudio's ASIO and WASAPI hosts
/// stop a stream in code that waits with no limit — `ASIOStop`, and a `SignalObjectAndWait(...,
/// INFINITE)` — and so a driver wedged in there held whichever thread had asked. That was the
/// window's: STOP, a device change, and since the outage handling reopens an input on its own a
/// second after the audio stops, a wedged driver froze the whole window, PANIC included, with
/// nothing anybody could press. A caller here waits as long as it chooses and then gets on with
/// it; the job goes on waiting for the driver on this thread instead.
///
/// **Why one thread, and not one per call**: an ASIO driver is a COM object, created in the
/// apartment of the thread that opens it — PortAudio leaves COM on any other thread to the
/// caller — and a driver may keep windows or timers of its own on that thread. So the thread
/// that opens a stream is the thread that starts, stops and closes it, and it is a
/// single-threaded apartment that **dispatches messages while it waits** for its next job, which
/// the window's thread used to do for the driver without anybody deciding it should. PortAudio
/// itself takes one call at a time, and every call being made here is what guarantees it.
class AudioThread {
public:
    /// Starts the thread; on Windows it joins a single-threaded COM apartment first.
    AudioThread();
    /// Lets whatever is queued finish, for a few seconds, and ends the thread. A thread still
    /// held by a driver is left to it — detached, with everything it uses kept alive by the
    /// thread itself — since there is nothing a destructor could do to bring it back.
    ~AudioThread();

    AudioThread(const AudioThread&) = delete;
    AudioThread& operator=(const AudioThread&) = delete;

    /// Runs `job` on the thread and waits up to `limit` for it. What it throws is rethrown here.
    ///
    /// True when it finished. **False when it did not finish in time**: it goes on running, and
    /// the thread is `stuck()` until it returns — which, with a wedged driver, may be never. And
    /// false at once, `job` never run, while the thread is stuck with an earlier job: a call
    /// queued behind a driver that does not return would never start, and the caller wants an
    /// answer now, not whenever the driver lets go.
    ///
    /// So `job` must not own anything that has to be destroyed on this thread: when it is
    /// refused it is destroyed where it was handed in. `handOver` is for that.
    ///
    /// `what` names the job, for `stuckOn()` and for `setJobHook`.
    bool run(const char* what, std::function<void()> job, std::chrono::milliseconds limit);

    /// The same, for a job that **has** to happen on this thread whenever it can — closing a
    /// stream, which must be done where it was opened, and done even if the driver takes an
    /// hour. Queued behind whatever the thread is stuck on rather than refused, and the thread
    /// owns `job` from here on: it is run, and destroyed, there. True when it had finished within
    /// `limit`; false when it had not, including when it is still waiting its turn.
    bool handOver(const char* what, std::function<void()> job, std::chrono::milliseconds limit);

    /// A job ran past the time its caller gave it and has not finished since.
    bool stuck() const;
    /// What that job was, or empty.
    std::string stuckOn() const;

    /// For the tests: called on the thread at the start of every job, with its name, before the
    /// job itself. A hook that blocks is a driver that does not return, which nothing on a test
    /// machine can produce on demand. Empty is none, which is the application.
    void setJobHook(std::function<void(const char*)> hook);

    /// The thread's id — for a test that checks a call really lands on it.
    std::thread::id id() const noexcept { return id_; }

private:
    struct State;
    /// The thread itself: jobs as they come, and messages between them.
    static void loop(const std::shared_ptr<State>& state);
    bool enqueue(const char* what, std::function<void()> job, std::chrono::milliseconds limit,
                 bool refuseWhenStuck);

    std::shared_ptr<State> state_;
    std::thread thread_;
    std::thread::id id_;
};

} // namespace takt4::audio
