#include "core/audio/audio_thread.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#endif

namespace takt4::audio {

namespace {

/// How long the destructor lets the jobs still queued finish before it leaves them to it.
constexpr std::chrono::seconds kFinishOnExit{5};

} // namespace

struct AudioThread::State {
    struct Job {
        std::string what;
        std::function<void()> fn;
        std::exception_ptr error;
        /// Refused rather than run if its caller has given up on it before it started: a `run`
        /// job's caller wanted an answer, and one that arrives later is an answer to nobody.
        bool refuseLate = false;
        bool started = false;
        bool done = false;
        /// Its caller stopped waiting before it finished.
        bool abandoned = false;
    };

    std::mutex mutex;
    std::condition_variable done;
    std::deque<std::shared_ptr<Job>> queue;
    /// The job the thread is in, from the moment it takes it until it has finished it.
    std::shared_ptr<Job> running;
    std::function<void(const char*)> hook;
    bool quit = false;
    bool exited = false;
#if defined(_WIN32)
    /// Auto-reset: set by whoever queues a job or asks the thread to finish, and waited on by
    /// the thread alongside its message queue.
    HANDLE wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~State() {
        if (wake != nullptr) {
            ::CloseHandle(wake);
        }
    }
#else
    std::condition_variable wakeCv;
#endif

    void signal() {
#if defined(_WIN32)
        ::SetEvent(wake);
#else
        wakeCv.notify_all();
#endif
    }
};

void AudioThread::loop(const std::shared_ptr<State>& state) {
#if defined(_WIN32)
    // A single-threaded apartment, as the window's thread was: the driver PortAudio creates here
    // lives in it, and gets its messages dispatched by the wait below.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    for (;;) {
        std::shared_ptr<State::Job> job;
        std::function<void(const char*)> hook;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            while (!state->queue.empty()) {
                job = std::move(state->queue.front());
                state->queue.pop_front();
                if (job->refuseLate && job->abandoned) {
                    // Given up on before it began: not run, and destroyed here like every job.
                    job->done = true;
                    job->fn = nullptr;
                    job.reset();
                    continue;
                }
                break;
            }
            if (job) {
                job->started = true;
                state->running = job;
                hook = state->hook;
            } else if (state->quit) {
                break;
            }
        }
        if (job) {
            try {
                if (hook) {
                    hook(job->what.c_str());
                }
                job->fn();
            } catch (...) {
                job->error = std::current_exception();
            }
            // **Destroyed here**, whatever it held: a stream opened on this thread is closed on
            // it too, by whichever of its owners lets go last.
            job->fn = nullptr;
            {
                const std::lock_guard<std::mutex> lock(state->mutex);
                job->done = true;
                state->running.reset();
            }
            state->done.notify_all();
            continue;
        }
#if defined(_WIN32)
        // Idle: wait for a job, and dispatch whatever a driver's windows and timers on this
        // thread are sent meanwhile — the window's thread did that for them before.
        const DWORD woke = ::MsgWaitForMultipleObjectsEx(1, &state->wake, INFINITE, QS_ALLINPUT,
                                                         MWMO_INPUTAVAILABLE);
        if (woke == WAIT_OBJECT_0 + 1) {
            MSG message;
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }
#else
        std::unique_lock<std::mutex> lock(state->mutex);
        state->wakeCv.wait(lock, [&] { return state->quit || !state->queue.empty(); });
#endif
    }
#if defined(_WIN32)
    if (SUCCEEDED(com)) {
        ::CoUninitialize();
    }
#endif
    {
        const std::lock_guard<std::mutex> lock(state->mutex);
        state->exited = true;
    }
    state->done.notify_all();
}

AudioThread::AudioThread() : state_(std::make_shared<State>()) {
    // The thread holds the state as well, so a thread left to a wedged driver by the destructor
    // still has everything it touches.
    thread_ = std::thread([state = state_] { loop(state); });
    id_ = thread_.get_id();
}

AudioThread::~AudioThread() {
    bool held = false;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->quit = true;
        held = state_->running && state_->running->abandoned;
    }
    state_->signal();
    std::unique_lock<std::mutex> lock(state_->mutex);
    // A driver already known to be holding the thread is not waited for again.
    const bool finished =
        !held && state_->done.wait_for(lock, kFinishOnExit, [this] { return state_->exited; });
    lock.unlock();
    if (finished) {
        thread_.join();
    } else {
        // A driver holding the thread. Nothing here can bring it back; it keeps its own share of
        // the state, and the process ends around it.
        thread_.detach();
    }
}

bool AudioThread::enqueue(const char* what, std::function<void()> fn,
                          std::chrono::milliseconds limit, bool refuseWhenStuck) {
    auto job = std::make_shared<State::Job>();
    job->what = what;
    job->fn = std::move(fn);
    job->refuseLate = refuseWhenStuck;
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (refuseWhenStuck && state_->running && state_->running->abandoned) {
        // Behind a job that has not come back, this would never start. Destroyed here — which a
        // `run` job is allowed to be; see there.
        lock.unlock();
        job->fn = nullptr;
        return false;
    }
    state_->queue.push_back(job);
    state_->signal();
    const bool finished = state_->done.wait_for(lock, limit, [&] { return job->done; });
    if (!finished) {
        job->abandoned = true;
        return false;
    }
    // **Taken out of the job under the lock**, so the exception lives and dies on this thread.
    // The audio thread drops its own share of the job when it moves on, and a job still holding
    // the exception then freed it there while this thread was reading its message — the first
    // linux-tsan run over this code (2026-09-30) reported exactly that.
    const std::exception_ptr error = std::exchange(job->error, nullptr);
    lock.unlock();
    if (error) {
        std::rethrow_exception(error);
    }
    return true;
}

bool AudioThread::run(const char* what, std::function<void()> job,
                      std::chrono::milliseconds limit) {
    return enqueue(what, std::move(job), limit, true);
}

bool AudioThread::handOver(const char* what, std::function<void()> job,
                           std::chrono::milliseconds limit) {
    return enqueue(what, std::move(job), limit, false);
}

bool AudioThread::stuck() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running && state_->running->abandoned;
}

std::string AudioThread::stuckOn() const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running && state_->running->abandoned ? state_->running->what : std::string{};
}

void AudioThread::setJobHook(std::function<void(const char*)> hook) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->hook = std::move(hook);
}

} // namespace takt4::audio
