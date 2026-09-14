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
// real fonts, the real colours — rendered by Slint, not a mock-up of it.
//
// The readouts are filled by running the tracker over the committed synthetic excerpt, so
// the picture shows numbers the engine actually produced rather than invented ones.
// `--stopped` renders the other state worth looking at: the idle window, which is what the
// app is the moment it opens and the only place the disabled controls can be seen.

#include "core/audio/devices.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/build_info.hpp"
#include "core/control/control_action.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/output/midi_ports.hpp"
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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace takt4::ui {
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
/// seconds of activation. `takt4-cli track tests/data/features/synthetic.wav` prints the
/// same run, so the picture and that line can be checked against each other.
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
    for (std::size_t h = 0; h < hops; ++h) {
        beatEngine->processHop(padded.data() + h * audio::kHopSize, h);
        (void)beatEngine->step();
        engine::EngineFrame frame;
        while (beatEngine->popFrame(frame)) {
            // The network's frames only, as the live window draws them; the shot engine is
            // the particle filter, which interpolates nothing, so this is for the day it is
            // not.
            if (frame.interpolated) {
                continue;
            }
            std::rotate(trace.begin(), trace.begin() + 1, trace.end());
            trace.back() = tracePoint(frame);
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
            for (int c = 0; c < chosen->maxInputChannels; ++c) {
                channels->push_back(slint::SharedString(describeChannel(*chosen, c)));
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

/// §5.9's editor, with a set of rules in it worth looking at.
///
/// Built here rather than through `RulesController`, because a controller needs an
/// `OutputRunner` — a Link session and three sockets, none of which a picture of a layout
/// has any business opening. What is drawn is the real component with the real models; only
/// where the values came from differs.
void fillRules(RulesWindow& window) {
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

    auto sources = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::LiveSource source : trigger::kLiveSources) {
        sources->push_back(slint::SharedString(std::string(trigger::labelOf(source))));
    }
    window.set_live_sources(sources);

    auto hosts = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* label : {"custom", "Resolume 7 - clip", "Resolume 7 - resync", "TouchDesigner",
                              "MadMapper - cue"}) {
        hosts->push_back(slint::SharedString(label));
    }
    window.set_host_presets(hosts);
    // Which preset the address below is, as `RulesController::presetOf` works it out — the
    // picker names the rule in front of it rather than the last thing clicked.
    window.set_host_preset_index(1);

    // No "add a preset..." entry at the front: the control is a menu with a fixed label now,
    // not a dropdown that has to sit on something. See `RulesController`'s `kRigPresets`.
    auto rigs = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* label : {"Resolume: clips on 3 layers", "Resolume: tempo and resync",
                              "Resolume: breathing dashboard", "MIDI: euclidean stabs"}) {
        rigs->push_back(slint::SharedString(label));
    }
    window.set_rig_presets(rigs);

    auto units = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const trigger::DelayUnit unit : trigger::kDelayUnits) {
        units->push_back(slint::SharedString(std::string(trigger::labelOf(unit))));
    }
    window.set_follow_up_units(units);

    auto shapes = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* label : {"saw", "triangle", "sine", "square"}) {
        shapes->push_back(slint::SharedString(label));
    }
    window.set_ramp_shapes(shapes);

    // Three rules, because a list of one says nothing about a list: one firing, one switched
    // off, and one that will not fire — the state §5.8 insists has to be *visible*.
    const auto rule = [](const char* name, bool enabled, const char* problem, int fires,
                         bool chosen = false) {
        RuleRow row{};
        row.name = slint::SharedString(name);
        row.enabled = enabled;
        row.problem = slint::SharedString(problem);
        row.fires = fires;
        row.chosen = chosen;
        return row;
    };
    auto rules = std::make_shared<slint::VectorModel<RuleRow>>();
    // Two chosen rather than one, because the selection is a *set* now and a picture of one
    // selected row says nothing about that — nor about the marks each chosen row carries.
    rules->push_back(rule("Layer 1 - random clip", true, "", 37, true));
    rules->push_back(rule("Layer 2 - random clip", true, "", 18, true));
    rules->push_back(rule("Layer 3 - random clip", false, "", 0));
    rules->push_back(rule("Dashboard breathes over 4 bars", true, "", 296));
    rules->push_back(rule("Euclidean stabs - 3 in 8", true, "", 111));
    rules->push_back(rule("Resync every 8 bars", true,
                          "the address has 1 templated segment and the rule has 0", 0));
    window.set_rules(rules);
    window.set_selected(0);

    // §5.6's rule subset, and what it currently reaches: the rig's own outputs, ticked.
    auto choices = std::make_shared<slint::VectorModel<OutputChoice>>();
    const auto choice = [](const char* name, bool chosen, bool missing) {
        OutputChoice row{};
        row.name = slint::SharedString(name);
        row.chosen = chosen;
        row.missing = missing;
        return row;
    };
    choices->push_back(choice("deck", true, false));
    choices->push_back(choice("wall", false, false));
    choices->push_back(choice("lights", false, false));
    window.set_output_choices(choices);
    window.set_outputs_all(false);
    window.set_outputs_summary(slint::SharedString("deck"));
    window.set_outputs_available(slint::SharedString("reaches 1 output"));

    window.set_rule_name(slint::SharedString("Layer 1 - random clip"));
    window.set_rule_enabled(true);
    window.set_trigger_index(1); // every N bars
    window.set_trigger_takes_every(true);
    window.set_every(4);
    window.set_min_confidence(0.70f);
    window.set_probability(0.9f);
    // Text, not numbers: both boxes are two-way bound so that they keep following the model
    // after somebody has typed into one. See `bpm-field` in the markup.
    window.set_bpm_range(slint::SharedString("120 - 140"));
    window.set_cooldown_ms(slint::SharedString("500"));
    window.set_allow_calm(false);
    window.set_send_index(0);
    window.set_sends_osc(true);
    window.set_address(slint::SharedString("/composition/layers/{layer}/clips/{clip}/connect"));

    // THEN SEND: two entries, because one would say nothing about it being a list. The first
    // is §5.6's release — the shape that used to be a tick box — and the second is the thing
    // the tick box could not say at all.
    auto followKinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    // The first entry names what it inherits — `RulesController::releaseLabelOf`. A picture
    // that still said the bare "release" would be a picture of the thing that misread.
    for (const char* label : {"release (same address)", "MIDI note", "MIDI note off", "MIDI CC",
                              "MIDI program", "MIDI pitch bend"}) {
        followKinds->push_back(slint::SharedString(label));
    }
    window.set_follow_kinds(followKinds);
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

    // The two chips of that address, and the value. The middle one is the sequence the whole
    // of `trigger::Pool` exists for: four clips the operator picked, shuffled.
    //
    // By name, not by position: §6 records that a Slint `export struct` becomes a C++ class
    // with its fields in declaration order, and that nothing promises that survives a Slint
    // bump. A thirteen-field aggregate is the last place to rely on it.
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

    slots->push_back(fixedSlot("value", "1", "1"));
    window.set_slots(slots);

    window.set_last_fired(slint::SharedString("/composition/layers/3/clips/7/connect 1"));
    window.set_last_fired_ago(slint::SharedString("2s ago"));

    auto log = std::make_shared<slint::VectorModel<slint::SharedString>>();
    for (const char* line : {"184s  drop  /composition/layers/3/clips/7/connect 1",
                             "184s  drop  /composition/layers/3/clips/7/connect 0",
                             "177s  drop  /composition/layers/3/clips/12/connect 1",
                             "177s  drop  /composition/layers/3/clips/12/connect 0",
                             "170s  drop  /composition/layers/3/clips/1/connect 1",
                             "170s  drop  /composition/layers/3/clips/1/connect 0",
                             "163s  drop  /composition/layers/3/clips/3/connect 1"}) {
        log->push_back(slint::SharedString(line));
    }
    window.set_log(log);
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

} // namespace

int renderShot(const std::filesystem::path& out, const ShotOptions& options) {
    if (options.width < 200 || options.height < 200) {
        std::cerr << "takt4-shot: the window is at least 200 x 200\n";
        return 2;
    }
    const int width = options.width;
    const int height = options.height;

    if (options.rules) {
        return renderWindow(out, width, height, [] {
            auto window = RulesWindow::create();
            fillRules(*window);
            return window;
        });
    }

    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);

    HeadlessWindow* const* rendered = installHeadlessPlatform(w, h);

    auto window = MainWindow::create();
    publishControlLimits(*window);
    window->set_tap_needs(3);
    fillPickers(*window);

    // The machine's real MIDI outputs, as the window offers them; the first entry is the
    // "nothing picked" label, exactly as WindowController builds it.
    auto midiPorts = std::make_shared<slint::VectorModel<slint::SharedString>>();
    midiPorts->push_back(slint::SharedString("no MIDI clock"));
    auto midiDevices = std::make_shared<slint::VectorModel<slint::SharedString>>();
    midiDevices->push_back(slint::SharedString("select a MIDI device"));
    for (const std::string& port : output::listMidiOutputPorts()) {
        midiPorts->push_back(slint::SharedString(port));
        midiDevices->push_back(slint::SharedString(port));
    }
    window->set_midi_ports(midiPorts);
    window->set_output_devices(midiDevices);

    auto outputKinds = std::make_shared<slint::VectorModel<slint::SharedString>>();
    outputKinds->push_back(slint::SharedString("OSC"));
    outputKinds->push_back(slint::SharedString("MIDI"));
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
        window->set_link_on(true);
        window->set_link_peers(2);
        window->set_osc_on(true);
        // §5.6's named targets, which is what the list holds now: a media server and a
        // lighting desk, each of which a rule can be aimed at by name, and one switched off
        // — the state a single field could only spell as "off " in front of an address.
        auto targets = std::make_shared<slint::VectorModel<OutputRow>>();
        const auto target = [](const char* name, const char* host, const char* port, bool enabled,
                               float delayMs = 0.0f) {
            OutputRow row{};
            row.name = slint::SharedString(name);
            row.kind_index = 0;
            row.host = slint::SharedString(host);
            row.port = slint::SharedString(port);
            row.address = slint::SharedString(std::string(host) + ":" + port);
            row.enabled = enabled;
            row.delay_ms = delayMs;
            return row;
        };
        targets->push_back(target("deck", "192.168.1.40", "7000", true));
        // One of each sign, because a rig where every destination shares a lag is the case
        // that never needed §5.6's per-target offset — and because the two directions do
        // different things underneath. The media server is nudged *early*, which for a
        // message about a beat already heard means "just before the next one"; the robot,
        // which has to physically move, is pushed late.
        targets->push_back(target("wall", "192.168.1.41", "7000", true, -80.0f));
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
        window->set_outputs_list(targets);
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
        window->set_osc_control_reading(
            slint::SharedString("/takt4/ctl/tap  from 192.168.1.40  9 acted, 0 ignored"));
        // §5.9's TRIGGERS row, with a rule set behind it: what the main window says about
        // the editor without the editor being open.
        window->set_rules_active(2);
        window->set_rules_total(3);
        window->set_rules_last_fired(
            slint::SharedString("/composition/layers/3/clips/7/connect 1"));
        window->set_status(slint::SharedString(
            "In 7 of MOTU Pro Audio  ·  48000 Hz -> 22050 Hz  ·  native pick  ·  "
            "latency 12.0 ms input + 16.4 ms resampler + 40.0 ms centred framing"));
    } else {
        // What the app looks like the moment it opens: no tempo, an empty trace, and
        // every one of §5.5's manual controls disabled because there is nothing yet for
        // them to correct.
        publishIdleReadouts(*window);
        publishTempoOptions(*window, tracking::TempoTracker::Options{});
        window->set_running(false);
        // What a freshly opened app shows: the port it *would* bind, and no socket. The
        // defaults, spelled out, because this picture is the one that shows an operator
        // what they are turning on.
        window->set_osc_control_port(slint::SharedString("7001"));
        window->set_osc_control_reading(slint::SharedString("off"));
        window->set_status(slint::SharedString("pick an input and press Start."));
    }
    window->set_status_is_error(false);
    // As the real app does it: the build belongs in the title bar and the status bar's
    // corner, where a status cannot take it away. A screenshot that did not carry it would
    // be a picture with no way of saying which build it is a picture of.
    window->set_version(slint::SharedString(buildInfo().version));

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

} // namespace takt4::ui
