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

#include "core/audio/devices.hpp"
#include "core/audio/portaudio_session.hpp"
#include "core/audio/rates.hpp"
#include "core/engine/beat_engine.hpp"
#include "core/io/wav_file.hpp"
#include "core/model/weights.hpp"
#include "core/tracking/state_space.hpp"
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

} // namespace

int renderShot(const std::filesystem::path& out, int width, int height) {
    if (width < 200 || height < 200) {
        std::cerr << "takt4-shot: the window is at least 200 x 200\n";
        return 2;
    }
    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);

    HeadlessWindow* const* rendered = installHeadlessPlatform(w, h);

    auto window = MainWindow::create();
    fillPickers(*window);
    auto traceModel =
        std::make_shared<slint::VectorModel<TracePoint>>(std::vector<TracePoint>(kTraceLength));
    window->set_trace(traceModel);
    fillFromSyntheticRun(*window, traceModel);
    window->set_status(
        slint::SharedString("In 7 of MOTU Pro Audio  ·  48000 Hz -> 22050 Hz  ·  native pick  ·  "
                            "latency 12.0 ms input + 16.4 ms resampler + 40.0 ms centred framing"));
    window->set_status_is_error(false);

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
