#include "ui/app.hpp"

#include "core/assets/embedded.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/io/utf8.hpp"
#include "core/model/weights.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/state_space.hpp"
#include "ui/crash_report.hpp"
#include "ui/native_window.hpp"
#include "ui/window_controller.hpp"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

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
    // Before anything else, so that anything after it which falls over leaves a minidump and a
    // sentence rather than a window that vanished (the audit's H16). Where the settings live,
    // because that is the folder an operator already knows about. A log left behind by the last
    // session is taken first, since installing starts a new one in its place.
    const std::filesystem::path home = settings::settingsDirectory();
    const std::filesystem::path leftover = CrashReport::takeLeftoverLog(home);
    CrashReport::install(home, environment("TAKT4_NO_CRASH_DIALOG").empty());

    // Whatever the last run left (Q7). Never fails: a settings file that is missing or
    // unreadable gives the defaults, because it must not be the reason the app will not
    // open. The tracker's own tuning goes in here rather than being applied afterwards,
    // so the engine is built with it and the window's sliders show it from the first
    // frame instead of jumping a moment later.
    //
    // Read from wherever the settings currently are and written back beside the executable,
    // which are the same place on every run but the first one after the move — that run
    // reads the old per-user file and, on the way out, leaves a settings.json next to
    // takt4.exe. See `settings::existingSettingsFile`.
    const std::filesystem::path settingsPath = settings::settingsFile();
    const std::filesystem::path readFrom = settings::existingSettingsFile();
    // **Through `openAtStartup`, never `load` alone.** `load` gives the defaults for a file it
    // cannot parse exactly as it does for a missing one, and the save on the way out then wrote
    // those defaults over the damaged file — so one trailing comma, or half a file left by a
    // crash, cost the whole rig (the audit's C8). This moves a damaged file aside, falls back
    // to the copy kept at the last good start, and says what it did.
    const settings::Startup startup = settings::openAtStartup(settingsPath, readFrom);
    const settings::Settings& saved = startup.settings;
    // Whether anything may be written to `settingsPath` without the operator asking: not when
    // the file there could be neither read nor moved aside, because a save would replace it.
    const bool writable = !settingsPath.empty() && startup.writable;

    // **A read from the old per-user file is a migration, so it is finished here rather than
    // on the way out.** `existingSettingsFile` falls back to `%APPDATA%\takt4\settings.json`
    // when there is none beside the executable, and until this the fallback was live: every
    // build into a fresh tree, and every session that ended in a crash rather than a clean
    // exit, read that file again. On this machine it was five days stale and had the octave
    // fold switched on with a window a tap had set — so "keep BPM in range" kept coming back
    // on with a window nobody had chosen, which is exactly what a rig reported on 2026-09-16.
    //
    // Writing it here means the fallback is taken once per install location and never again.
    // Nothing is moved or deleted: the old file stays where an older build still finds it,
    // which is the decision `existingSettingsFile` already documents.
    //
    // A recovery is written straight away for the same reason: the damaged file has been moved
    // aside, and the settings takt4 is now running on — the backup's, or the defaults — should
    // be what a crash in the next minute leaves behind.
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
    std::unique_ptr<engine::LiveTracker> tracker;
    try {
        tracker = std::make_unique<engine::LiveTracker>(
            model::ModelWeights::fromBytes(assets::weights(), "electronic (built in)"),
            tracking::StateSpaceModel::fromBytes(assets::stateSpace(), "default (built in)"),
            options);
    } catch (const std::exception& e) {
        // Where somebody will see it: takt4 is a window application on Windows and has no
        // console to write to unless it was started from one. See `ui::reportFatal`.
        reportFatal(e.what());
        return 1;
    }

    // Guarded like the tracker above. Nothing the saved settings name should be able to throw
    // out of here any more — outputs and ports are applied after the window exists, and a bad
    // OSC prefix is refused on the way in (the audit's C1) — but if something still does, the
    // operator gets a sentence rather than a process that vanished before its window appeared.
    // Every window opens no larger than this screen has room for (the audit's M26). Said here,
    // by the application alone, so the tests' windows keep the sizes they are written against.
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
    }
    controller->showNotice(notice);
    // Written a few seconds after each change from here on, so a crash, a power cut or a
    // shutdown with the window open costs seconds rather than the session.
    if (writable) {
        controller->enableAutosave(settingsPath);
    }
    // The bench switch that proves crash reporting on the machine it has to work on: from a
    // timer, so the failure happens inside Slint's event loop with the window up — where every
    // real one so far has happened — rather than before it.
    if (const std::string how = environment("TAKT4_TEST_CRASH"); !how.empty()) {
        slint::Timer::single_shot(std::chrono::milliseconds(1500),
                                  [how] { CrashReport::crashOnPurpose(how); });
    }
    controller->run();
    // The window has gone; stop the audio before the tracker goes with it.
    tracker->stop();

    // And once more on the way out, for whatever changed in the last few seconds.
    if (writable && !settings::save(controller->currentSettings(), settingsPath)) {
        reportFatal("Could not write " + io::pathText(settingsPath) +
                    ", so this session's settings were not kept.");
    }
    controller.reset();
    tracker.reset();
    // The session ended as it should, so its log goes: one still there at the next launch is
    // what "did not close normally" means.
    CrashReport::cleanExit();
    return 0;
}

} // namespace takt4::ui
