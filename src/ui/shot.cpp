// takt4-shot — render the main window to an image with no window system underneath it.
//
// HANDOFF §5.9's window is developed from a tool session that has a window station but no
// display: `CreateDC("DISPLAY", …)` returns null, so `BitBlt` from a screen DC fails and
// `PrintWindow` hands back a white client area whichever renderer is selected. Measured
// both ways. That left nobody able to see the window without walking to the machine.
//
// Slint's `SoftwareRenderer` does not need any of that. A custom `slint::platform::Platform`
// hands the runtime a window adapter whose renderer draws into a buffer we own, and the
// buffer is written out as a BMP. The result is the real component — the real layout, the
// real fonts, the real colors — rendered by Slint, not a mock-up of it.
//
// The readouts are filled by running the tracker over the committed synthetic excerpt, so
// the picture shows numbers the engine actually produced rather than invented ones.
// `--stopped` renders the other state worth looking at: the idle window, which is what the
// app is the moment it opens and the only place the disabled controls can be seen.

#include "core/audio/devices.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/audio/stereo_check.hpp"
#include "core/build_info.hpp"
#include "core/control/control_action.hpp"
#include "core/dmx/effect.hpp"
#include "core/dmx/fixture.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/midi_ports.hpp"
#include "core/output/output_target.hpp"
#include "core/settings/settings.hpp"
#include "core/tracking/state_space.hpp"
#include "core/tracking/tempo_tracker.hpp"
#include "core/trigger/generator.hpp"
#include "core/trigger/rule.hpp"
#include "ui/app.hpp"
#include "ui/headless.hpp"
#include "ui/window_state.hpp"

#include "main_window.h" // generated from main_window.slint

#include <slint-platform.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace takt4::ui {

// A picture at the size each window opens at (app.hpp), which is the size C++ gives it
// (window_state.hpp). Held here so the two cannot drift apart again (the audit of 2026-09-25, L34).
static_assert(ShotOptions{}.width == static_cast<int>(kMainWindowWidth) &&
              ShotOptions{}.height == static_cast<int>(kMainWindowHeight));
static_assert(kRulesShotWidth == static_cast<int>(kRulesWindowWidth) &&
              kRulesShotHeight == static_cast<int>(kRulesWindowHeight));
static_assert(kFixturesShotWidth == static_cast<int>(kFixturesWindowWidth) &&
              kFixturesShotHeight == static_cast<int>(kFixturesWindowHeight));
namespace {

/// 24-bit BMP, bottom-up, rows padded to four bytes. A BMP because it needs no library
/// and every tool reads one; converting to PNG afterwards is a one-liner anywhere.
void writeBmp(const std::filesystem::path& path, const std::vector<slint::Rgb8Pixel>& pixels,
              int width, int height) {
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const std::size_t rowBytes = w * 3;
    const std::size_t padding = (4 - (rowBytes % 4)) % 4;
    const std::size_t imageBytes = (rowBytes + padding) * h;
    constexpr std::size_t kHeaderBytes = 54;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error(path.string() + ": cannot create");
    }

    const auto put32 = [&out](std::uint32_t value) {
        const char bytes[4] = {
            static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
            static_cast<char>((value >> 16) & 0xFF), static_cast<char>((value >> 24) & 0xFF)};
        out.write(bytes, 4);
    };
    const auto put16 = [&out](std::uint16_t value) {
        const char bytes[2] = {static_cast<char>(value & 0xFF),
                               static_cast<char>((value >> 8) & 0xFF)};
        out.write(bytes, 2);
    };

    out.write("BM", 2);
    put32(static_cast<std::uint32_t>(kHeaderBytes + imageBytes));
    put32(0);
    put32(static_cast<std::uint32_t>(kHeaderBytes));
    put32(40); // BITMAPINFOHEADER
    put32(static_cast<std::uint32_t>(width));
    put32(static_cast<std::uint32_t>(height));
    put16(1);  // planes
    put16(24); // bits per pixel
    put32(0);  // BI_RGB, no compression
    put32(static_cast<std::uint32_t>(imageBytes));
    put32(2835); // 72 dpi
    put32(2835);
    put32(0);
    put32(0);

    const std::vector<char> pad(padding, '\0');
    for (std::size_t y = 0; y < h; ++y) {
        const std::size_t row = h - 1 - y; // BMP rows run bottom to top
        for (std::size_t x = 0; x < w; ++x) {
            const slint::Rgb8Pixel& pixel = pixels[row * w + x];
            const char bgr[3] = {static_cast<char>(pixel.b), static_cast<char>(pixel.g),
                                 static_cast<char>(pixel.r)};
            out.write(bgr, 3);
        }
        if (padding != 0) {
            out.write(pad.data(), static_cast<std::streamsize>(padding));
        }
    }
    if (!out) {
        throw std::runtime_error(path.string() + ": write failed");
    }
}

/// Runs the committed synthetic excerpt through the engine and leaves the window holding
/// what came out: the tempo it settled on, the lock, the bar it was in, and the last four
/// seconds of activation. `takt4-cli track tests/data/features/synthetic.wav --weights
/// generic` prints the same run, so the picture and that line can be checked against each
/// other.
void fillFromSyntheticRun(MainWindow& window,
                          const std::shared_ptr<slint::VectorModel<TracePoint>>& traceModel) {
    const std::filesystem::path wav =
        std::filesystem::path(TAKT4_TEST_DATA_DIR) / "features" / "synthetic.wav";
    const io::WavData audio = io::readWavFile(wav);
    const model::ModelWeights weights =
        model::ModelWeights::fromFile(std::filesystem::path(TAKT4_WEIGHTS_DIR) / "generic.bin");
    const tracking::StateSpaceModel space = tracking::StateSpaceModel::fromFile(
        std::filesystem::path(TAKT4_STATESPACE_DIR) / "default.bin");

    const std::size_t hops = (audio.samples.size() + audio::kHopSize - 1) / audio::kHopSize;
    if (hops == 0) {
        throw std::runtime_error(wav.string() + ": no audio to run the tracker over");
    }
    std::vector<float> padded(hops * audio::kHopSize, 0.0f);
    std::copy(audio.samples.begin(), audio.samples.end(), padded.begin());

    // Not called `engine`: that would read as the namespace two lines down. Qualified
    // lookup would still find the namespace — it only considers namespaces and types —
    // but a reader should not have to know that rule to follow this.
    auto beatEngine = std::make_unique<engine::BeatEngine>(weights, space);
    std::vector<TracePoint> trace(kTraceLength);
    bool beatPending = false;
    for (std::size_t h = 0; h < hops; ++h) {
        beatEngine->processHop(padded.data() + h * audio::kHopSize, h);
        (void)beatEngine->step();
        engine::EngineFrame frame;
        while (beatEngine->popFrame(frame)) {
            // The network's frames only, as the live window draws them, and a beat that
            // landed on one the engine interpolated carried to the next — the shot's engine is
            // the forward filter, the default, at twice the network's rate. This said it was
            // the particle filter and dropped those beats (the audit of 2026-09-25).
            if (frame.interpolated) {
                beatPending = beatPending || frame.beat;
                continue;
            }
            std::rotate(trace.begin(), trace.begin() + 1, trace.end());
            TracePoint point = tracePoint(frame);
            point.called = point.called || beatPending;
            beatPending = false;
            trace.back() = point;
        }
        engine::EngineBeat beat;
        while (beatEngine->popBeat(beat)) {
        }
    }

    // The level the last hop of the excerpt actually carried, so the meter is not invented.
    float sumSquares = 0.0f;
    float peak = 0.0f;
    for (std::size_t i = (hops - 1) * audio::kHopSize; i < hops * audio::kHopSize; ++i) {
        sumSquares += padded[i] * padded[i];
        peak = std::max(peak, std::abs(padded[i]));
    }
    const float rms = std::sqrt(sumSquares / static_cast<float>(audio::kHopSize));

    for (std::size_t i = 0; i < trace.size(); ++i) {
        traceModel->set_row_data(i, trace[i]);
    }
    // Pinned, so the picture shows the one control that has an on-state: every other
    // button looks the same lit or not, and this is the only way to see that a held LOCK
    // reads as held rather than as merely another dark rectangle.
    (void)beatEngine->post(engine::Command::setLockPinned(true));
    (void)beatEngine->step();

    publishTempoState(window, beatEngine->state());
    publishTempoOptions(window, beatEngine->tempoOptions());
    publishInput(window, rms, peak);
    window.set_running(true);
}

/// The device and channel pickers, filled from the machine this is running on so the
/// picture shows the real list rather than a placeholder.
void fillPickers(MainWindow& window) {
    auto devices = std::make_shared<slint::VectorModel<slint::SharedString>>();
    auto channels = std::make_shared<slint::VectorModel<slint::SharedString>>();
    try {
        const audio::PortAudioSession session;
        const std::vector<audio::InputDevice> found = audio::listInputDevices(session);
        const audio::InputDevice* chosen = nullptr;
        for (const audio::InputDevice& device : found) {
            devices->push_back(slint::SharedString(describeDevice(device)));
            if (chosen == nullptr && !device.isLoopback &&
                audio::hasNativeChannelSelection(device.hostApi)) {
                chosen = &device;
            }
        }
        if (chosen == nullptr && !found.empty()) {
            chosen = &found.front();
        }
        if (chosen != nullptr) {
            // Pairs, as the window lists them unless "mono" is ticked (2026-09-28).
            for (int c = 0; c < chosen->maxInputChannels; c += 2) {
                channels->push_back(slint::SharedString(c + 1 < chosen->maxInputChannels
                                                            ? describePair(*chosen, c)
                                                            : describeChannel(*chosen, c)));
            }
            for (std::size_t i = 0; i < found.size(); ++i) {
                if (found[i].index == chosen->index) {
                    window.set_device_index(static_cast<int>(i));
                }
            }
        }
    } catch (const std::exception&) {
        // A machine with no audio at all still gets a picture; the pickers are just empty.
    }
    window.set_devices(devices);
    window.set_channels(channels);
    window.set_channel_index(0);
}

/// The lighting patch editor, with the rig the approved mockup of 2026-09-30 holds
/// (`design/weltformat-dark/patch1.html`): two washes, two heads — one left out of the show — and a
/// blinder that runs off the end of its universe, which between them exercise every branch the
/// list has. `state` is "" or "fit" (the 16-bit head picked), "par" (the LED par picked, which
/// cannot move, so no C), "none" (nothing patched) or "message" (the head, with a line under the
/// top row).
void fillFixtures(FixturesWindow& window, const std::string& state) {
    auto roles = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Role role : dmx::kRoles) {
        roles->push_back(slint::SharedString(std::string(dmx::labelOf(role))));
    }
    window.set_roles(roles);

    // As `FixturesController` words them.
    auto modes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    modes->push_back(slint::SharedString("custom (the channels below)"));
    for (const dmx::FixtureMode& mode : dmx::builtinModes()) {
        modes->push_back(slint::SharedString(std::string(mode.name)));
    }
    window.set_modes(modes);
    window.set_test_level(255);
    window.set_test_seconds(3);

    if (state == "none") {
        window.set_fixtures(std::make_shared<slint::VectorModel<FixtureRow>>());
        window.set_selected(-1);
        window.set_channels(std::make_shared<slint::VectorModel<ChannelRow>>());
        window.set_moves(false);
        window.set_summary(slint::SharedString("Nothing patched yet."));
        return;
    }

    const auto row = [](const char* name, const char* where, const char* group, bool enabled,
                        const char* problem) {
        FixtureRow one{};
        one.name = slint::SharedString(name);
        one.where = slint::SharedString(where);
        one.group = slint::SharedString(group);
        one.enabled = enabled;
        one.problem = slint::SharedString(problem);
        return one;
    };
    auto fixtures = std::make_shared<slint::VectorModel<FixtureRow>>();
    fixtures->push_back(row("wash L", "0 · 1-4", "washes", true, ""));
    fixtures->push_back(row("wash R", "0 · 5-10", "washes", true, ""));
    fixtures->push_back(row("head 1", "0 · 11-22", "heads", true, ""));
    fixtures->push_back(row("head 2", "0 · 23-34", "heads", false, ""));
    // One that cannot be driven, because that is the state the list has to be able to show.
    fixtures->push_back(
        row("blinder", "0 · 505-516", "", true, "runs off the end of the universe"));
    window.set_fixtures(fixtures);
    window.set_summary(slint::SharedString("5 fixtures on 1 universe, going to 1 node"));
    window.set_universe(slint::SharedString("0"));
    window.set_enabled(true);

    const bool par = state == "par";
    const std::size_t mode = par ? 4 : 6; // "LED par (6ch)", "moving head 16-bit (12ch)"
    const dmx::Fixture fixture =
        dmx::fixtureFromMode(par ? "wash R" : "head 1", mode, 0, par ? 5 : 11);
    window.set_selected(par ? 1 : 2);
    window.set_name(slint::SharedString(fixture.name));
    window.set_group(slint::SharedString(par ? "washes" : "heads"));
    window.set_address(static_cast<int>(fixture.address));
    window.set_mode_index(static_cast<int>(mode) + 1); // after the "custom" entry
    window.set_moves(!par);
    window.set_pan_min(10);
    window.set_pan_max(90);
    window.set_tilt_min(20);
    window.set_tilt_max(70);
    if (state == "message") {
        window.set_status(slint::SharedString("Channel 18 at 255 for 3 s."));
        window.set_status_error(false);
    }

    auto channels = std::make_shared<slint::VectorModel<ChannelRow>>();
    // Levels that look like a fixture part way through a move, so the live bars have something
    // to draw — which is the one thing in this window a static picture cannot otherwise show.
    const std::array<int, 12> headLive{164, 32, 96, 200, 0, 190, 255, 255, 64, 0, 0, 0};
    const std::array<int, 6> parLive{204, 255, 32, 64, 0, 0};
    for (std::size_t i = 0; i < fixture.channels.size(); ++i) {
        ChannelRow one{};
        one.number = static_cast<int>(fixture.address) + static_cast<int>(i);
        for (std::size_t r = 0; r < dmx::kRoles.size(); ++r) {
            if (dmx::kRoles[r] == fixture.channels[i]) {
                one.role_index = static_cast<int>(r);
                break;
            }
        }
        one.parked = static_cast<int>(fixture.parked[i]);
        one.live = par ? (i < parLive.size() ? parLive[i] : 0) : (i < headLive.size() ? headLive[i] : 0);
        channels->push_back(one);
    }
    window.set_channels(channels);
}

/// §5.9's editor, holding exactly what the locked mockup of 2026-09-30 holds (HANDOFF §0.5,
/// `design/weltformat-dark/rules17.html`), so a render and the approved picture can be laid over
/// each other — and, with `state`, the other states its pictures show.
///
/// Built here rather than through `RulesController`, because a controller needs an
/// `OutputRunner` — a Link session and three sockets, none of which a picture of a layout has any
/// business opening. What is drawn is the real component with the real models; only where the
/// values came from differs.
void fillRules(RulesWindow& window, bool dmx, bool panicked, const std::string& state) {
    const auto strings = [](std::initializer_list<const char*> items) {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (const char* item : items) {
            model->push_back(slint::SharedString(item));
        }
        return model;
    };
    auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::Trigger which : trigger::kTriggers) {
        names->push_back(slint::SharedString(std::string(trigger::labelOf(which))));
    }
    window.set_trigger_names(names);
    auto sends = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::Message::Kind kind : trigger::kMessageKinds) {
        sends->push_back(slint::SharedString(std::string(trigger::labelOf(kind))));
    }
    window.set_send_kinds(sends);
    auto kinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::GeneratorKind kind : trigger::kGeneratorKinds) {
        kinds->push_back(slint::SharedString(std::string(trigger::labelOf(kind))));
    }
    window.set_generator_kinds(kinds);
    // As `RulesController` has them: a color's chip offers only what can make a color.
    auto colorKinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::GeneratorKind kind : trigger::kColorGeneratorKinds) {
        colorKinds->push_back(slint::SharedString(std::string(trigger::labelOf(kind))));
    }
    window.set_color_generator_kinds(colorKinds);
    auto sources = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::LiveSource source : trigger::kLiveSources) {
        sources->push_back(slint::SharedString(std::string(trigger::labelOf(source))));
    }
    window.set_live_sources(sources);
    window.set_host_presets(strings({"custom", "Resolume 7 - clip", "Resolume 7 - resync",
                                     "TouchDesigner", "MadMapper - cue"}));
    window.set_host_preset_index(1);
    window.set_rig_presets(strings({"Resolume: clips on 3 layers", "Resolume: tempo and resync",
                                    "Resolume: breathing dashboard", "MIDI: euclidean stabs"}));
    auto units = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::DelayUnit unit : trigger::kDelayUnits) {
        units->push_back(slint::SharedString(std::string(trigger::labelOf(unit))));
    }
    window.set_follow_up_units(units);
    window.set_ramp_shapes(strings({"saw", "triangle", "sine", "square"}));

    // The list: one picked, one muted, one switched off, two with counts, and one that will not
    // fire — every state a row has (the mockup's own six, and the lighting half's seventh).
    const auto rule = [](const char* name, bool enabled, const char* problem, int fires,
                         bool chosen, bool muted) {
        RuleRow row{};
        row.name = slint::SharedString(name);
        row.enabled = enabled;
        row.problem = slint::SharedString(problem);
        row.fires = fires;
        row.chosen = chosen;
        row.muted = muted;
        return row;
    };
    auto rules = std::make_shared<slint::VectorModel<RuleRow>>();
    rules->push_back(rule("Layer 1 - random clip", true, "", 412, !dmx, false));
    rules->push_back(rule("Layer 2 - random clip", true, "", 180, false, true));
    rules->push_back(rule("Layer 3 - random clip", false, "", 0, false, false));
    rules->push_back(rule("Dashboard breathes over 4 bars", true, "", 296, false, false));
    rules->push_back(rule("Euclidean stabs - 3 in 8", true, "", 111, false, false));
    if (dmx) {
        rules->push_back(rule("Heads change color on the drop", true, "", 37, true, false));
    }
    // Worded as `Rule::problem` words the commonest: a new MIDI rule before its number is picked.
    rules->push_back(rule("Laser stab on the drop", true, "choose a note number", 0, false, false));
    window.set_rules(rules);
    window.set_selected(dmx ? 5 : 0);
    window.set_panicked(panicked);

    // §5.6's rule subset: the rig's own outputs, one ticked, one it names that is gone.
    const auto choice = [](const char* key, const char* name, bool chosen, bool missing) {
        OutputChoice row{};
        row.key = slint::SharedString(key);
        row.name = slint::SharedString(name);
        row.chosen = chosen;
        row.missing = missing;
        return row;
    };
    auto choices = std::make_shared<slint::VectorModel<OutputChoice>>();
    choices->push_back(choice("o-1", "deck", true, false));
    choices->push_back(choice("o-2", "wall", false, false));
    choices->push_back(choice("o-3", "resolume 2", false, false));
    window.set_output_choices(choices);
    window.set_outputs_all(false);
    window.set_outputs_summary(slint::SharedString("deck"));
    window.set_outputs_available(slint::SharedString("reaches 1 output"));

    window.set_rule_name(slint::SharedString("Layer 1 - random clip"));
    window.set_rule_id(slint::SharedString("layer1"));
    window.set_rule_enabled(true);
    window.set_trigger_index(1); // bars
    window.set_trigger_takes_every(true);
    window.set_trigger_takes_cooldown(false);
    window.set_every(4);
    window.set_cooldown_ms(slint::SharedString("250"));
    window.set_when_summary(slint::SharedString("every 4 bars, counting from the first"));

    // B, switched off and folded, as a new rule's is — and what it holds, set for later.
    window.set_conditions_on(false);
    window.set_only_if_folded(true);
    window.set_min_confidence(0.70f);
    window.set_probability(0.9f);
    window.set_bpm_range(slint::SharedString("120 - 140"));
    window.set_allow_calm(false);
    window.set_only_if_summary(slint::SharedString("off — fires every time A comes round"));

    window.set_send_index(0);
    window.set_sends_osc(true);
    window.set_send_value(true);
    window.set_address(slint::SharedString("/composition/layers/{layer}/clips/{clip}/connect"));
    window.set_send_summary(
        slint::SharedString("OSC to deck · /composition/layers/{layer}/clips/{clip}/connect"));

    auto effects = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::EffectKind kind : dmx::kEffectKinds) {
        effects->push_back(slint::SharedString(std::string(dmx::labelOf(kind))));
    }
    window.set_effect_kinds(effects);
    auto effectRoles = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Role role : dmx::kAimableRoles) {
        effectRoles->push_back(slint::SharedString(std::string(dmx::labelOf(role))));
    }
    window.set_effect_roles(effectRoles);
    auto curves = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::Curve curve : dmx::kCurves) {
        curves->push_back(slint::SharedString(std::string(dmx::labelOf(curve))));
    }
    window.set_effect_curves(curves);
    auto paths = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const dmx::PathShape shape : dmx::kPathShapes) {
        paths->push_back(slint::SharedString(std::string(dmx::labelOf(shape))));
    }
    window.set_effect_shapes(paths);
    auto colorModes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::ColorMode mode : trigger::kColorModes) {
        colorModes->push_back(slint::SharedString(std::string(trigger::labelOf(mode))));
    }
    window.set_color_modes(colorModes);

    // THEN SEND: §5.6's release, and the thing the old tick box could not say at all.
    window.set_follow_kinds(strings({"release (same address)", "MIDI note", "MIDI note off",
                                     "MIDI CC", "MIDI program", "MIDI pitch bend"}));
    const auto owed = [](int kind, const char* numberLabel, int number, const char* valueLabel,
                         const char* value, int unit, const char* delay, const char* summary) {
        FollowRow row{};
        row.kind_index = kind;
        row.takes_number = *numberLabel != '\0';
        row.number_label = slint::SharedString(numberLabel);
        row.number = number;
        row.takes_value = true;
        row.value_label = slint::SharedString(valueLabel);
        row.value = slint::SharedString(value);
        row.unit = unit;
        row.delay = slint::SharedString(delay);
        row.summary = slint::SharedString(summary);
        return row;
    };
    auto follows = std::make_shared<slint::VectorModel<FollowRow>>();
    follows->push_back(owed(0, "", 0, "value", "0", 1, "1", "the same address"));
    follows->push_back(owed(3, "cc", 21, "value", "64", 2, "2", ""));
    window.set_follow_ups(follows);
    window.set_then_summary(slint::SharedString("release after 1 beat · MIDI CC 21 after 2 bars"));

    // The two chips of that address, and the value: the sequence `trigger::Pool` exists for.
    // By name, not by position: nothing promises a Slint struct's field order survives a bump.
    const auto fixedSlot = [](const char* label, const char* value, const char* last) {
        SlotRow row{};
        row.label = slint::SharedString(label);
        row.kind_index = 4; // Fixed
        row.fixed = slint::SharedString(value);
        row.is_fixed = true;
        row.last = slint::SharedString(last);
        return row;
    };
    auto slots = std::make_shared<slint::VectorModel<SlotRow>>();
    slots->push_back(fixedSlot("{layer}", "3", "3"));
    SlotRow clip{};
    clip.label = slint::SharedString("{clip}");
    clip.kind_index = 0; // Shuffle
    clip.takes_pool = true;
    clip.is_list = true;
    clip.low = 1;
    clip.high = 8;
    clip.values = slint::SharedString("3, 7, 1, 12");
    clip.no_repeat = 2;
    clip.last = slint::SharedString("7");
    slots->push_back(clip);
    SlotRow value = fixedSlot("value", "1", "1");
    value.is_osc_value = true;
    slots->push_back(value);

    if (dmx) {
        // A color on the moving heads, drawn from a palette: the color mode, the swatches, and
        // the rate a surface has doubled.
        window.set_rule_name(slint::SharedString("Heads change color on the drop"));
        window.set_rule_id(slint::SharedString("heads-drop"));
        window.set_send_index(6); // DMX / Art-Net, last of `kMessageKinds`
        window.set_sends_osc(false);
        window.set_sends_midi(false);
        window.set_sends_dmx(true);
        window.set_effect_index(1); // color
        window.set_effect_takes_role(false);
        window.set_effect_takes_curve(true);
        window.set_effect_takes_color(true);
        window.set_color_mode_index(0); // pick colors
        window.set_effect_duration(slint::SharedString("2"));
        window.set_effect_unit(2);        // bars
        window.set_effect_curve_index(0); // linear
        window.set_rule_muted(true);
        window.set_rule_rate(slint::SharedString("2× faster"));
        window.set_send_summary(slint::SharedString("color on heads, lasers"));

        auto palette = std::make_shared<slint::VectorModel<PaletteEntry>>();
        for (const trigger::Value& entry : trigger::defaultPalette()) {
            std::string text;
            entry.appendTo(text);
            const dmx::Color color = dmx::parseColor(text).value_or(dmx::kWhite);
            PaletteEntry swatch{};
            swatch.swatch = slint::Color::from_rgb_uint8(color.r, color.g, color.b);
            swatch.hex = slint::SharedString(text);
            double hue = 0.0;
            double saturation = 1.0;
            double brightness = 1.0;
            dmx::toHsv(color, hue, saturation, brightness);
            swatch.hue = static_cast<float>(hue);
            swatch.sat = static_cast<float>(saturation * 100.0);
            swatch.val = static_cast<float>(brightness * 100.0);
            palette->push_back(swatch);
        }
        window.set_palette(palette);
        window.set_palette_shown(true);

        auto lights = std::make_shared<slint::VectorModel<OutputChoice>>();
        lights->push_back(choice("heads", "heads", true, false));
        lights->push_back(choice("washes", "washes", false, false));
        lights->push_back(choice("lasers", "lasers", true, false));
        lights->push_back(choice("f-3", "spot 3", false, true));
        window.set_fixture_choices(lights);
        window.set_fixtures_summary(slint::SharedString("heads, lasers"));
        window.set_fixtures_available(slint::SharedString("reaches 2 fixtures"));
        window.set_fixtures_list_note(slint::SharedString("reaches 2 fixtures"));

        slots = std::make_shared<slint::VectorModel<SlotRow>>();
        SlotRow color = fixedSlot("color", "#ff2040", "#20ff80");
        color.is_color = true;
        color.swatch = slint::Color::from_rgb_uint8(0xff, 0x20, 0x40);
        color.hue = 348;
        color.sat = 87;
        color.val = 100;
        color.is_fixed = false;
        color.kind_index = 0; // shuffle
        color.takes_pool = true;
        color.is_list = true;
        color.no_repeat = 1;
        slots->push_back(color);
    }
    window.set_slots(slots);

    auto log = std::make_shared<slint::VectorModel<LogLine>>();
    const auto line = [](const char* when, const char* who, const char* message, bool muted) {
        LogLine row{};
        row.when = slint::SharedString(when);
        row.rule = slint::SharedString(who);
        row.message = slint::SharedString(message);
        row.muted = muted;
        return row;
    };
    // Named as `RulesController::tick` names them: by the rule's name, as the list shows it.
    log->push_back(line("184s", "Layer 1 - random clip", "/composition/layers/3/clips/7/connect 1", false));
    log->push_back(line("183s", "Layer 1 - random clip", "/composition/layers/3/clips/7/connect 0", false));
    log->push_back(line("177s", "Layer 2 - random clip", "/composition/layers/2/clips/4/connect 1", true));
    log->push_back(line("170s", "Dashboard breathes over 4 bars", "/dashboard/breathe 0.5", false));
    log->push_back(line("170s", "Euclidean stabs - 3 in 8", "/stabs/hit 1", false));
    log->push_back(line("163s", "Layer 1 - random clip", "/composition/layers/3/clips/12/connect 1", false));
    log->push_back(line("162s", "Layer 1 - random clip", "/composition/layers/3/clips/12/connect 0", false));
    log->push_back(line("156s", "Euclidean stabs - 3 in 8", "/stabs/hit 1", false));
    window.set_log(log);

    // --- the other states ---------------------------------------------------------------------
    if (state == "message") {
        window.set_status(slint::SharedString("A range is two numbers, like 1 - 12."));
        window.set_status_is_error(true);
    } else if (state == "onset") {
        // A "when" that comes in bursts: no count, so ÷2 and ×2 switched off, and the cooldown live.
        window.set_trigger_index(6);
        window.set_trigger_takes_every(false);
        window.set_trigger_takes_cooldown(true);
        window.set_when_summary(slint::SharedString("onset · at most once every 250 ms"));
    } else if (state == "log") {
        window.set_log_open(true);
    } else if (state == "folded") {
        window.set_when_folded(true);
        window.set_send_folded(true);
        window.set_then_folded(true);
        window.set_conditions_on(true);
        window.set_only_if_summary(
            slint::SharedString("confidence over 0.7 · 90% · normal, intense · 120 - 140 BPM"));
    } else if (state == "b-on" || state == "b-off") {
        window.set_only_if_folded(false);
        window.set_conditions_on(state == "b-on");
    } else if (state == "fit") {
        window.set_only_if_folded(false);
        window.set_conditions_on(true);
        window.set_log_open(true);
    } else if (state == "none") {
        // As the controller leaves it with no rule picked: no slots, no follow-ups, nothing in the
        // name box but what to do.
        window.set_selected(-1);
        window.set_rule_name(slint::SharedString(""));
        window.set_rule_enabled(false);
        window.set_slots(std::make_shared<slint::VectorModel<SlotRow>>());
        window.set_follow_ups(std::make_shared<slint::VectorModel<FollowRow>>());
        window.set_address(slint::SharedString(""));
        window.set_when_summary(slint::SharedString(""));
        window.set_only_if_summary(slint::SharedString(""));
        for (std::size_t i = 0; i < rules->row_count(); ++i) {
            RuleRow row = *rules->row_data(i);
            row.chosen = false;
            rules->set_row_data(i, row);
        }
    } else if (state == "no-lights") {
        window.set_fixtures_summary(slint::SharedString("nothing — sends nowhere"));
        window.set_fixtures_available(
            slint::SharedString("pick at least one — a DMX rule with no fixtures does nothing"));
        window.set_fixtures_list_note(slint::SharedString("pick at least one"));
        window.set_fixtures_reaches_nothing(true);
    }
}

/// Renders whatever component `build` returns, and writes it out — the half of `renderShot`
/// that does not care which of the two windows it is looking at.
template <typename Build>
int renderWindow(const std::filesystem::path& out, int width, int height, Build build) {
    HeadlessWindow* const* rendered = installHeadlessPlatform(static_cast<std::uint32_t>(width),
                                                              static_cast<std::uint32_t>(height));

    auto window = build();
    // show() creates the adapter; the two dispatches give the scene its scale and size,
    // which nothing else would do without a window manager to hear from.
    window->show();
    window->window().dispatch_scale_factor_change_event(1.0f);
    window->window().dispatch_resize_event(
        slint::LogicalSize({static_cast<float>(width), static_cast<float>(height)}));

    if (*rendered == nullptr) {
        std::cerr << "takt4-shot: the platform was never asked for a window\n";
        return 1;
    }
    std::vector<slint::Rgb8Pixel> pixels(static_cast<std::size_t>(width) *
                                         static_cast<std::size_t>(height));
    (*rendered)->software().render(pixels, static_cast<std::size_t>(width));
    writeBmp(out, pixels, width, height);

    std::cout << "takt4-shot: " << width << " x " << height << " written to " << out.string()
              << '\n';
    return 0;
}

/// What the approved mockup of the Weltformat-dark redesign holds (HANDOFF §0.5,
/// `design/weltformat-dark/main.html` and `data.js`), over a window already filled as running —
/// so that a render and the approved picture can be laid over each other. The trace is the
/// synthetic run's, which is where the mockup's trace was lifted from.
void fillAsMockup(MainWindow& window) {
    const auto strings = [](std::initializer_list<const char*> items) {
        auto model = std::make_shared<slint::VectorModel<slint::SharedString>>();
        for (const char* item : items) {
            model->push_back(slint::SharedString(item));
        }
        return model;
    };
    window.set_devices(strings({"ASIO / MOTU Pro Audio (18 in)"}));
    window.set_device_index(0);
    window.set_channels(strings({"In 1 + 2 \xE2\x80\x94 Mic 2 / Mic 2"}));
    window.set_channel_index(0);
    window.set_input_mono(false);

    window.set_bpm(128.29f);
    window.set_called_bpm(128.29f);
    window.set_raw_bpm(128.29f);
    window.set_refined(true);
    window.set_locked(true);
    window.set_holding(false);
    window.set_input_lost(false);
    window.set_confidence(0.62f);
    window.set_beats_per_bar(4);
    window.set_beat_in_bar(2);
    window.set_bars(6);
    window.set_input_level(0.63f);
    window.set_input_peak(0.5f);
    window.set_input_reading(slint::SharedString("-21.7 dB"));
    window.set_fold_on(true);
    window.set_fold_min(70.0f);
    window.set_fold_max(140.0f);
    window.set_latency_ms(0.0f);
    window.set_keep_shift(false);
    window.set_pinned(true);
    window.set_tap_count(0);

    window.set_midi_in_ports(strings({"select input"}));
    window.set_midi_in_port_index(0);
    window.set_control_on(true);
    window.set_control_error(false);
    window.set_control_reading(slint::SharedString("note 36 ch 10"));

    window.set_output_devices(strings(
        {"select a MIDI device", "Microsoft GS Wavetable Synth", "MOTU Pro Audio Midi Out 1"}));
    auto rows = std::make_shared<slint::VectorModel<OutputRow>>();
    OutputRow link{};
    link.name = slint::SharedString("Link");
    link.kind_index = 4;
    link.enabled = true;
    rows->push_back(link);
    const auto clock = [](const char* name, int device, float delayMs) {
        OutputRow row{};
        row.name = slint::SharedString(name);
        row.kind_index = 3;
        row.device_index = device;
        row.enabled = true;
        row.delay_ms = delayMs;
        return row;
    };
    rows->push_back(clock("DAW clock", 1, -12.0f));
    rows->push_back(clock("TR-8S", 2, 25.0f));
    const auto osc = [](const char* name, const char* host, float delayMs) {
        OutputRow row{};
        row.name = slint::SharedString(name);
        row.kind_index = 0;
        row.host = slint::SharedString(host);
        row.port = slint::SharedString("7000");
        row.enabled = true;
        row.delay_ms = delayMs;
        return row;
    };
    rows->push_back(osc("deck", "192.168.1.40", 0.0f));
    rows->push_back(osc("wall", "192.168.1.41", -80.0f));
    window.set_outputs_list(rows);
    window.set_link_peers(2);
    window.set_link_peers_shown(true);
    window.set_beats_sent(22);
    window.set_fixtures_total(5);
    window.set_rules_active(2);
    window.set_rules_total(3);
}

} // namespace

int renderShot(const std::filesystem::path& out, const ShotOptions& options) {
    if (options.width < 200 || options.height < 200) {
        std::cerr << "takt4-shot: the window is at least 200 x 200\n";
        return 2;
    }
    const int width = options.width;
    const int height = options.height;

    if (options.rules) {
        return renderWindow(out, width, height, [&options] {
            auto window = RulesWindow::create();
            fillRules(*window, options.dmx, options.panicked, options.state);
            return window;
        });
    }

    if (options.fixtures) {
        return renderWindow(out, width, height, [&options] {
            auto window = FixturesWindow::create();
            fillFixtures(*window, options.state);
            return window;
        });
    }

    if (options.widgets) {
        return renderWindow(out, width, height, [] {
            auto window = WidgetBench::create();
            auto rows = std::make_shared<slint::VectorModel<BenchRow>>();
            rows->push_back(BenchRow{slint::SharedString("deck"), true, 0, 0.0f});
            rows->push_back(BenchRow{slint::SharedString("wall"), false, 3, -40.0f});
            window->set_rows(rows);
            auto names = std::make_shared<slint::VectorModel<slint::SharedString>>();
            for (int i = 1; i <= 30; ++i) {
                names->push_back(slint::SharedString("device " + std::to_string(i)));
            }
            window->set_long_list(names);
            window->set_tick_a(true);
            window->set_track_value(40.0f);
            return window;
        });
    }

    if (options.about) {
        return renderWindow(out, width, height, [] {
            auto window = AboutWindow::create();
            window->set_version(slint::SharedString(versionLabel(buildInfo())));
            window->set_opened(slint::SharedString(
                "Opened C:\\Users\\someone\\AppData\\Local\\Temp\\takt4\\takt4-THIRD-PARTY-NOTICES.txt"));
            return window;
        });
    }

    // Physical pixels: the logical size at the display's scale.
    const int physicalWidth =
        static_cast<int>(std::lround(width * static_cast<double>(options.scale)));
    const int physicalHeight =
        static_cast<int>(std::lround(height * static_cast<double>(options.scale)));
    const auto w = static_cast<std::uint32_t>(physicalWidth);
    const auto h = static_cast<std::uint32_t>(physicalHeight);

    HeadlessWindow* const* rendered = installHeadlessPlatform(w, h);

    auto window = MainWindow::create();
    publishControlLimits(*window);
    window->set_tap_needs(3);
    fillPickers(*window);

    // The machine's real MIDI outputs, as the window offers them; the first entry is the
    // "nothing picked" label, exactly as WindowController builds it.
    auto midiDevices = std::make_shared<slint::VectorModel<slint::SharedString>>();
    midiDevices->push_back(slint::SharedString("select a MIDI device"));
    for (const std::string& port : output::listMidiOutputPorts()) {
        midiDevices->push_back(slint::SharedString(port));
    }
    window->set_output_devices(midiDevices);

    auto outputKinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    outputKinds->push_back(slint::SharedString("OSC"));
    outputKinds->push_back(slint::SharedString("MIDI"));
    outputKinds->push_back(slint::SharedString("Art-Net"));
    outputKinds->push_back(slint::SharedString("MIDI clock"));
    window->set_output_kinds(outputKinds);

    // §5.7's control row, built the way WindowController builds it.
    auto midiInputs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    midiInputs->push_back(slint::SharedString("select input"));
    for (const std::string& port : output::listMidiInputPorts()) {
        midiInputs->push_back(slint::SharedString(port));
    }
    window->set_midi_in_ports(midiInputs);
    auto learnActions = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const control::ControlAction action : control::kControlActions) {
        // The same list the window builds, and skipping the same one: a gesture cannot say
        // which rule `rule/<id>/enable` means. See `WindowController`'s constructor.
        if (control::takesRuleId(action)) {
            continue;
        }
        learnActions->push_back(slint::SharedString(std::string(control::labelOf(action))));
    }
    window->set_learn_actions(learnActions);

    auto traceModel =
        std::make_shared<slint::VectorModel<TracePoint>>(std::vector<TracePoint>(kTraceLength));
    window->set_trace(traceModel);
    if (options.running) {
        fillFromSyntheticRun(*window, traceModel);
        // §5.9's own example row, so the picture shows what a configured rig looks like
        // rather than an empty one. There is no output thread behind a shot, so these are
        // illustrative in the way the status line below already is; the beat count is the
        // excerpt's real 21.
        window->set_link_peers(2);
        // §5.6's named targets, which is what the list holds now: a media server and a
        // lighting desk, each of which a rule can be aimed at by name, and one switched off
        // — the state a single field could only spell as "off " in front of an address.
        auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
        // Link and the MIDI clock, first and in the same shape as everything else: a switch, a
        // name, a delay. Link's destination is the peers it found; the clock's is a MIDI device.
        OutputRow link{};
        link.name = slint::SharedString("Link");
        link.kind_index = 4;
        link.enabled = true;
        targets->push_back(link);
        // Its peers, as SHOW PEERS lists them, open: one in takt4's session and one that is not.
        auto peers = std::make_shared<slint::VectorModel<LinkPeer>>();
        peers->push_back(LinkPeer{slint::SharedString("192.168.1.20"),
                                  slint::SharedString("128.29 BPM"), true,
                                  slint::SharedString("playing")});
        peers->push_back(LinkPeer{slint::SharedString("192.168.1.35"),
                                  slint::SharedString("120.00 BPM"), false,
                                  slint::SharedString("")});
        window->set_link_peer_list(peers);
        window->set_link_peers_shown(true);
        // Two MIDI clocks, each to its own device: a DAW and a drum machine.
        OutputRow clock{};
        clock.name = slint::SharedString("DAW clock");
        clock.kind_index = 3;
        clock.device_index = midiDevices->row_count() > 1 ? 1 : 0;
        clock.enabled = true;
        clock.delay_ms = -12.0f;
        targets->push_back(clock);
        OutputRow drums{};
        drums.name = slint::SharedString("TR-8S");
        drums.kind_index = 3;
        drums.device_index = midiDevices->row_count() > 2 ? 2 : 0;
        drums.enabled = true;
        drums.delay_ms = 25.0f;
        targets->push_back(drums);
        const auto target = [](const char* name, const char* host, const char* port, bool enabled,
                               float delayMs = 0.0f, bool own = false) {
            OutputRow row{};
            row.name = slint::SharedString(name);
            row.kind_index = 0;
            row.host = slint::SharedString(host);
            row.port = slint::SharedString(port);
            row.address = slint::SharedString(std::string(host) + ":" + port);
            row.enabled = enabled;
            row.sends_namespace = own;
            row.delay_ms = delayMs;
            return row;
        };
        targets->push_back(target("deck", "192.168.1.40", "7000", true, 0.0f, true));
        // One of each sign, because a rig where every destination shares a lag is the case
        // that never needed §5.6's per-target offset — and because the two directions do
        // different things underneath. The media server is nudged *early*, which for a
        // message about a beat already heard means "just before the next one"; the robot,
        // which has to physically move, is pushed late — and is left out of takt4's own
        // messages, which its bridge does not want, as the other two are not.
        targets->push_back(target("wall", "192.168.1.41", "7000", true, -80.0f, true));
        targets->push_back(target("robot", "192.168.1.42", "7000", true, 352.0f));
        // The other kind of destination, so the picture shows both shapes of the row: a MIDI
        // target picks its device from a list rather than holding a typed name.
        OutputRow lights{};
        lights.name = slint::SharedString("lights");
        lights.kind_index = 1;
        lights.device_index = midiDevices->row_count() > 1 ? 1 : 0;
        lights.address = slint::SharedString("midi MOTU Midi Out 1");
        lights.enabled = false;
        targets->push_back(lights);
        // And the Art-Net shape: a node, a host and its port, fed every universe the patch uses.
        OutputRow truss{};
        truss.name = slint::SharedString("truss");
        truss.kind_index = 2;
        truss.host = slint::SharedString("10.0.0.20");
        truss.port = slint::SharedString("6454");
        truss.address = slint::SharedString("artnet 10.0.0.20:6454");
        truss.enabled = true;
        truss.delay_ms = 40.0f;
        targets->push_back(truss);
        window->set_outputs_list(targets);
        window->set_fixtures_total(5);
        window->set_beats_sent(21);
        // A control surface bound, so the row shows what a learned binding reads as
        // rather than an empty picker and a blank line.
        window->set_control_on(true);
        window->set_learn_action_index(0);
        window->set_control_reading(slint::SharedString("note 36 ch 10"));
        // §5.7's other surface, listening and being driven — the state worth looking at,
        // since "off" is two blank fields and says nothing about the layout.
        window->set_osc_control_on(true);
        window->set_osc_control_port(slint::SharedString("7001"));
        window->set_osc_control_reading(slint::SharedString("/takt4/ctl/tap"));
        window->set_osc_control_from(slint::SharedString("from 192.168.1.40"));
        window->set_osc_control_counts(slint::SharedString("9 acted, 0 ignored"));
        // §5.9's TRIGGERS row, with a rule set behind it: what the main window says about
        // the editor without the editor being open.
        window->set_rules_active(2);
        window->set_rules_total(3);
        window->set_rules_last_fired(
            slint::SharedString("/composition/layers/3/clips/7/connect 1"));
        window->set_status(slint::SharedString(
            "In 7 of MOTU Pro Audio \xC2\xB7 48000 Hz \xE2\x86\x92 22050 Hz \xC2\xB7 native pick\n"
            "latency 12.0 ms input + 16.4 ms resampler + 40.0 ms centred framing"));
        if (options.trouble) {
            // What each thing that cannot be reached says under itself, in the words the window
            // really uses (`output::midiPortBusyMessage`, `net::bindFailure`): a drum machine's
            // clock held by a DAW, a wall whose name will not resolve, and both control inputs
            // asked for and not open.
            for (std::size_t i = 0; i < targets->row_count(); ++i) {
                OutputRow row = *targets->row_data(i);
                if (std::string(row.name) == "TR-8S") {
                    row.problem = slint::SharedString(output::midiPortBusyMessage("TR-8S"));
                } else if (std::string(row.name) == "wall") {
                    row.host = slint::SharedString("wall.local");
                    row.problem =
                        slint::SharedString("looking up \"wall.local\" failed: no such host is known");
                } else {
                    continue;
                }
                targets->set_row_data(i, row);
            }
            window->set_control_on(false);
            // The picker names the controller asked for, as the window's own list does.
            if (const auto inputs = window->get_midi_in_ports()) {
                auto withMissing = std::make_shared<slint::VectorModel<slint::SharedString>>();
                for (std::size_t i = 0; i < inputs->row_count(); ++i) {
                    withMissing->push_back(*inputs->row_data(i));
                }
                withMissing->push_back(
                    slint::SharedString("nanoKONTROL2 \xE2\x80\x94 not plugged in"));
                window->set_midi_in_ports(withMissing);
                window->set_midi_in_port_index(static_cast<int>(withMissing->row_count()) - 1);
            }
            window->set_control_error(true);
            window->set_control_reading(slint::SharedString(
                "NOT OPEN \xE2\x80\x94 \"nanoKONTROL2\" is not on this machine \xE2\x80\x94 plug it "
                "in. Trying again every 5 s."));
            window->set_osc_control_error(true);
            // A stereo pair with a leg wired backwards, as `superviseStereo` says it.
            window->set_input_trouble(slint::SharedString(audio::StereoCheck::describe(
                audio::StereoCheck::Verdict::OutOfPhase, "In 11", "In 12")));
            window->set_osc_control_reading(slint::SharedString(
                "NOT LISTENING \xE2\x80\x94 port 7001 is in use by another program (10048). Trying "
                "again every 5 s."));
        }
    } else {
        // What the app looks like the moment it opens: no tempo, an empty trace, and
        // every one of §5.5's manual controls disabled because there is nothing yet for
        // them to correct.
        //
        // **As a fresh install opens, not as the tracker's own defaults** (the audit of
        // 2026-09-25, L34): the tempo options a settings file with nothing in it gives —
        // "keep BPM in" off — and the one output such an app has, Link, switched off. This drew
        // the fold on and no outputs at all: a table with no headings under a "nothing is being
        // sent" line the app never shows.
        publishIdleReadouts(*window);
        publishTempoOptions(*window, settings::freshTempoOptions());
        {
            std::vector<output::OutputTarget> outputs = settings::Settings{}.preset.outputs;
            (void)output::ensureLinkOutput(outputs, settings::Settings{}.preset.link);
            auto rows = std::make_shared<slint::VectorModel<OutputRow>>();
            for (const output::OutputTarget& target : outputs) {
                OutputRow row{};
                row.id = slint::SharedString(target.id);
                row.name = slint::SharedString(target.name);
                row.kind_index = static_cast<int>(target.kind);
                row.address = slint::SharedString(output::formatOutputAddress(target));
                row.enabled = target.enabled;
                row.sends_namespace = target.sendsNamespace;
                rows->push_back(row);
            }
            window->set_outputs_list(rows);
        }
        window->set_running(false);
        // What a freshly opened app shows: the port it *would* bind, and no socket. The
        // defaults, spelled out, because this picture is the one that shows an operator
        // what they are turning on.
        window->set_osc_control_port(slint::SharedString("7001"));
        window->set_osc_control_reading(slint::SharedString("off"));
        window->set_osc_control_reading_words(true);
        window->set_status(slint::SharedString("pick an input and press Start."));
    }
    window->set_status_is_error(false);
    if (options.mockup && options.running) {
        fillAsMockup(*window);
    }
    // What the controller counts for the folded Outputs heading, counted the same way.
    if (const auto rows = window->get_outputs_list()) {
        int on = 0;
        int failing = 0;
        std::vector<OutputRow> all;
        for (std::size_t i = 0; i < rows->row_count(); ++i) {
            const OutputRow row = *rows->row_data(i);
            on += row.enabled ? 1 : 0;
            failing += row.problem.empty() ? 0 : 1;
            all.push_back(row);
        }
        window->set_outputs_on(on);
        window->set_outputs_failing(failing);
        publishGlobalMessages(*window, all);
    }
    window->set_inputs_folded(options.foldInputs);
    window->set_outputs_folded(options.foldOutputs);
    window->set_panicked(options.panicked);
    // As the real app does it: the build belongs in the title bar and the status bar's
    // corner, where a status cannot take it away. A screenshot that did not carry it would
    // be a picture with no way of saying which build it is a picture of.
    window->set_version(slint::SharedString(versionLabel(buildInfo())));
    window->set_version_short(
        slint::SharedString(shortVersionLabel(buildInfo().version, buildInfo().commit)));
    if (options.mockup && options.running) {
        // A release's label, as the mockup has — this build's version, as its release would
        // show it (the README's picture is this render).
        const std::string version(buildInfo().version);
        window->set_version_short(slint::SharedString(shortVersionLabel(version, "v" + version)));
    }

    // show() creates the adapter; the two dispatches give the scene its scale and size,
    // which nothing else would do without a window manager to hear from.
    window->show();
    window->window().dispatch_scale_factor_change_event(options.scale);
    window->window().dispatch_resize_event(
        slint::LogicalSize({static_cast<float>(width), static_cast<float>(height)}));

    if (*rendered == nullptr) {
        std::cerr << "takt4-shot: the platform was never asked for a window\n";
        return 1;
    }
    std::vector<slint::Rgb8Pixel> pixels(static_cast<std::size_t>(physicalWidth) *
                                         static_cast<std::size_t>(physicalHeight));
    (*rendered)->software().render(pixels, static_cast<std::size_t>(physicalWidth));
    writeBmp(out, pixels, physicalWidth, physicalHeight);

    std::cout << "takt4-shot: " << physicalWidth << " x " << physicalHeight << " written to "
              << out.string() << '\n';
    return 0;
}

} // namespace takt4::ui
