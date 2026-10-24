#include "ui/app.hpp"

#include "core/assets/embedded.hpp"
#include "core/audio/audio_thread.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/io/utf8.hpp"
#include "core/model/weights.hpp"
#include "core/rt/thread_priority.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/state_space.hpp"
#include "ui/crash_report.hpp"
#include "ui/native_window.hpp"
#include "ui/single_instance.hpp"
#include "ui/window_controller.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace takt4::ui {

namespace {

/// An environment variable's value, or empty.
std::string environment(const char* name) {
#if defined(_MSC_VER)
    // Not std::getenv: MSVC deprecates it, and the buffer it returns is not ours.
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* const value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

} // namespace

int run() {
    const std::filesystem::path home = settings::settingsDirectory();
    // **One takt4 per settings folder** (`SingleInstance`): a second launch brings the first to
    // the front and goes. First of all, since everything after it is what two of them fight
    // over — this log among them, which a second launch truncated.
    SingleInstance instance(home);
    if (!instance.claimed()) {
        // Asked again for a few seconds: the first may be between its splash and its window.
        for (int tries = 0; tries < 25; ++tries) {
            if (SingleInstance::bringOtherToFront(home)) {
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        reportFatal("takt4 is already running from " + io::pathText(home) +
                    ", and its window is still opening.");
        return 0;
    }
    // Before anything else, so that anything after it which falls over leaves a minidump and a
    // sentence rather than a window that vanished. Where the settings live, because that is the
    // folder an operator already knows about. A log left behind by the last session is taken
    // first, since installing starts a new one in its place.
    const std::filesystem::path leftover = CrashReport::takeLeftoverLog(home);
    // Ended before it had settled: perhaps from what the settings asked for. The copy of them
    // kept at the last good start is kept as it is, rather than replaced with what may be why.
    const bool endedEarly = !leftover.empty() && CrashReport::endedEarly(leftover);
    CrashReport::install(home, environment("TAKT4_NO_CRASH_DIALOG").empty()
                                   ? CrashReport::kAfterCrashOption
                                   : std::string_view{});
    CrashReport::pruneReports(home);
    // Full speed whether the window is in front, behind or minimised — see `rt::keepFullSpeed`.
    (void)rt::keepFullSpeed();

    // Whatever the last run left. Never fails: a settings file that is missing or
    // unreadable gives the defaults, because it must not be the reason the app will not
    // open. The tracker's own tuning goes in here rather than being applied afterwards,
    // so the engine is built with it and the window's sliders show it from the first
    // frame instead of jumping a moment later.
    //
    // Read from beside the executable and written back there. See `settings::existingSettingsFile`
    // for why the old per-user file is not read any more.
    const std::filesystem::path settingsPath = settings::settingsFile();
    const std::filesystem::path readFrom = settings::existingSettingsFile();
    // **Through `openAtStartup`, never `load` alone.** `load` gives the defaults for a file it
    // cannot parse exactly as it does for a missing one, and the save on the way out then wrote
    // those defaults over the damaged file — so one trailing comma, or half a file left by a
    // crash, cost the whole rig. This moves a damaged file aside, falls back to the copy kept
    // at the last good start, and says what it did.
    const settings::Startup startup = settings::openAtStartup(settingsPath, readFrom, endedEarly);
    const settings::Settings& saved = startup.settings;
    // Whether anything may be written to `settingsPath` without the operator asking: not when
    // the file there could be neither read nor moved aside, because a save would replace it.
    const bool writable = !settingsPath.empty() && startup.writable;

    // **A recovery is written straight away**: the damaged file has been moved aside, and the
    // settings takt4 is now running on — the backup's, or the defaults — should be what a crash
    // in the next minute leaves behind. (`readFrom` was the old per-user file on a folder's
    // first run until 2026-10-08, and this finished that move at once; it is always
    // `settingsPath` now.)
    if (writable && (readFrom != settingsPath || !startup.notice.empty())) {
        (void)settings::save(saved, settingsPath);
    }

    engine::LiveTracker::Options options;
    options.engine.tempo = saved.preset.tempo;
    options.engine.decoder = saved.preset.decoder;
    options.engine.forward.meters = saved.preset.meters;

    // The assets come out of the executable itself (`core/assets/embedded.hpp`), so there is
    // no folder to find and nothing to go missing when somebody copies takt4.exe somewhere
    // else — which is the whole point of embedding them. They are still validated, and still
    // before the window opens: a checksum only fails here if the binary itself is damaged,
    // and that is worth saying plainly rather than crashing in the model a second later.
    //
    // Made on a thread of its own behind a splash, because making it lists the audio drivers —
    // up to half a minute with ASIO — and nothing at all was on screen meanwhile, so takt4 was
    // launched again. The splash keeps this thread's messages going while it waits.
    std::unique_ptr<engine::LiveTracker> tracker;
    std::string failed;
    {
        StartupSplash splash(
            "takt4 is starting \xE2\x80\x94 listing the audio interfaces\xE2\x80\xA6");
        // What a second launch brings forward meanwhile, since the window is not made yet.
        (void)instance.announceWindow(splash.window());
        std::thread make([&tracker, &failed, &options] {
            try {
                tracker = std::make_unique<engine::LiveTracker>(
                    model::ModelWeights::fromBytes(assets::weights(), "electronic (built in)"),
                    tracking::StateSpaceModel::fromBytes(assets::stateSpace(),
                                                         "default (built in)"),
                    options);
            } catch (const std::exception& e) {
                failed = e.what();
            } catch (...) {
                failed = "the audio could not be brought up";
            }
        });
        splash.waitFor(make);
    }
    if (!tracker) {
        // Where somebody will see it: takt4 is a window application on Windows and has no
        // console to write to unless it was started from one. See `ui::reportFatal`.
        reportFatal(failed);
        return 1;
    }

    // Guarded like the tracker above. Nothing the saved settings name should be able to throw
    // out of here any more — outputs and ports are applied after the window exists, and a bad
    // OSC prefix is refused on the way in — but if something still does, the operator gets a
    // sentence rather than a process that vanished before its window appeared.
    // Every window opens no larger than this screen has room for. Said here, by the application
    // alone, so the tests' windows keep the sizes they are written against.
    fitWindowsToScreen(true);
    std::unique_ptr<WindowController> controller;
    try {
        controller = std::make_unique<WindowController>(*tracker, saved);
    } catch (const std::exception& e) {
        reportFatal(std::string("takt4 could not open its window: ") + e.what());
        return 1;
    }
    // Last, so nothing the constructor said is on top of it: this is the one message about the
    // rig file itself, and the operator has to read it before they trust what is on screen.
    std::string notice = startup.notice;
    if (!leftover.empty()) {
        notice += std::string(notice.empty() ? "" : " ") +
                  "takt4 did not close normally last time; what it last printed is in " +
                  io::pathText(leftover.filename()) + ".";
        std::error_code missing;
        const std::filesystem::path backup = settings::backupFile(settingsPath);
        if (endedEarly && std::filesystem::exists(backup, missing)) {
            notice += " It ended within half a minute of opening its window. If the settings are "
                      "why, " +
                      io::pathText(backup.filename()) +
                      " is the copy kept when takt4 last started cleanly, left as it was: IMPORT "
                      "it to go back to it.";
        }
    }
    controller->showNotice(notice);
    // Written a few seconds after each change from here on, so a crash, a power cut or a
    // shutdown with the window open costs seconds rather than the session — or, for a file
    // takt4 must not write unasked, from the first SAVE to it.
    if (writable) {
        controller->enableAutosave(settingsPath);
    } else if (!settingsPath.empty()) {
        controller->holdAutosave(settingsPath);
    }
    // The bench switch that proves crash reporting on the machine it has to work on: from a
    // timer, so the failure happens inside Slint's event loop with the window up — where every
    // real one so far has happened — rather than before it.
    if (const std::string how = environment("TAKT4_TEST_CRASH"); !how.empty()) {
        slint::Timer::single_shot(std::chrono::milliseconds(1500),
                                  [how] { CrashReport::crashOnPurpose(how); });
    }
    // **Windows ending the session** ends the process once the window has answered, and nothing
    // below would run: so what the way out does for the settings and the rig is done there and
    // then, and the process ended without asking the audio driver anything — the machine is
    // going down around it, and a driver is what an exit can hang in.
    struct Ending {
        WindowController* controller = nullptr;
        const std::filesystem::path* path = nullptr;
        bool writable = false;
    };
    Ending ending{controller.get(), &settingsPath, writable};
    whenSessionEnds(
        [](void* user) {
            const auto& end = *static_cast<const Ending*>(user);
            if (end.writable || end.controller->autosaving()) {
                (void)settings::save(end.controller->currentSettings(), *end.path);
            }
            end.controller->endSession();
            CrashReport::cleanExit();
#if defined(_WIN32)
            ::TerminateProcess(::GetCurrentProcess(), 0);
#endif
        },
        &ending);
    // Half a minute up: what a session that ends from here on is not taken to have ended early.
    slint::Timer::single_shot(std::chrono::seconds(30), [] { CrashReport::markSettled(); });
    // The window this takt4 is, for a second launch to bring to the front — once it is up, which
    // is some turns of the event loop after it is shown: asked again until it is.
    slint::Timer announcing;
    announcing.start(slint::TimerMode::Repeated, std::chrono::milliseconds(200),
                     [&instance, &announcing] {
                         if (instance.announceWindow()) {
                             announcing.stop();
                         }
                     });
    controller->run();
    whenSessionEnds(nullptr, nullptr);
    // Once more on the way out, for whatever changed in the last few seconds — before the audio
    // is touched: closing the interface is where a driver's rare crash on close happens, and it
    // took those seconds with it.
    if ((writable || controller->autosaving()) &&
        !settings::save(controller->currentSettings(), settingsPath)) {
        reportFatal("Could not write " + io::pathText(settingsPath) +
                    ", so this session's settings were not kept.");
    }
    // The window has gone; stop the audio before the tracker goes with it.
    tracker->stop();
    controller.reset();
    tracker.reset();
    // Asked once the tracker has gone, since ending PortAudio there can itself be what a driver
    // never returns from: whether one is holding a thread of this process.
    const bool driverHeld = audio::AudioThread::leftToADriver();
    // The session ended as it should, so its log goes: one still there at the next launch is
    // what "did not close normally" means.
    CrashReport::cleanExit();
    if (driverHeld) {
        // A driver that stopped answering still holds the audio thread, and an ordinary exit
        // would go on to tell that driver's DLL it is being unloaded — which may wait on
        // whatever the driver is holding, and so never finish. Everything worth keeping has been
        // kept by now: the settings saved above, the lights blacked out by the controller on its
        // way. So the process ends here rather than asking the driver.
#if defined(_WIN32)
        ::TerminateProcess(::GetCurrentProcess(), 0);
#else
        std::_Exit(0);
#endif
    }
    return 0;
}

} // namespace takt4::ui
