#include "ui/app.hpp"

#include "core/assets/embedded.hpp"
#include "core/engine/live_tracker.hpp"
#include "core/model/weights.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/state_space.hpp"
#include "ui/native_window.hpp"
#include "ui/window_controller.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>

namespace takt4::ui {

int run() {
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
    const settings::Settings saved = settings::load(readFrom);

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
    if (!settingsPath.empty() && readFrom != settingsPath) {
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

    WindowController controller(*tracker, saved);
    controller.run();
    // The window has gone; stop the audio before the tracker goes with it.
    tracker->stop();

    // On the way out rather than on every change: a slider drag would otherwise write the
    // file sixty times a second. What is lost to a crash is one session's tweaks, which is
    // the right trade for a file nobody is waiting on.
    if (!settingsPath.empty() && !settings::save(controller.currentSettings(), settingsPath)) {
        reportFatal("Could not write " + settingsPath.string() +
                    ", so this session's settings were not kept.");
    }
    return 0;
}

} // namespace takt4::ui
