# takt4

A desktop application that listens to one selectable channel of a multi-channel audio
interface, tracks beat, downbeat, tempo and meter in real time, and broadcasts that
timing over Ableton Link, OSC and MIDI clock — with a GUI-configurable rule engine for
firing events on the beat.

Aimed at live production in general: VJ software, lighting desks, media servers, DAWs,
generative visuals. Windows, macOS and Linux.

## Status

Early. The build system, dependencies and CI are in place, and the audio path exists:
one channel (or a summed pair) of any input device is opened, resampled to the engine's
22050 Hz and cut into 20 ms hops. The feature front end that feeds the beat tracker —
madmom's log-filterbank spectrogram and its positive differences, 288 values per hop —
is implemented in C++ and verified against madmom itself on eighteen excerpts under
`tests/data/features/`: no filterbank value anywhere differs by more than one float32
ulp. Nothing is tracked yet. `takt4 --version` prints what it was built with, and the
window opens.

### Development console

`takt4-cli` is built alongside the application (in `bin/` next to it) but never
packaged. It exercises the engine without the UI:

```sh
takt4-cli devices                       # host APIs, then every input device and its channels
takt4-cli meter --device 1 --channel 7  # open input 7 of device 1, print RMS/peak at 10 Hz
takt4-cli meter --device 1 --channels 7,8
takt4-cli meter --device 1 --all        # every channel of the device, unresampled
takt4-cli features in.wav out.npy --compare golden.npy   # feature front end on a file
```

Channel numbers count from 1, as printed on the interface. On ASIO and CoreAudio the
selected channel is opened natively (the stream carries only that channel); elsewhere,
or with `--software`, the whole device is opened and the channel is sliced out in
software. `--rate HZ` overrides the device's default rate and `--seconds S` stops
without Ctrl-C. Debug builds carry a real-time allocation guard that aborts on any heap
use from the audio callback; `meter` reports whether it is on.

`features` runs a mono 22050 Hz WAV through the C++ feature front end and writes the
result as a numpy `.npy` file; with `--compare` it reports the largest difference to a
reference file and fails above 1e-5, the same check the test suite makes against the
golden excerpts (see [tests/data/features/README.md](tests/data/features/README.md)).

### Python tooling

`tools/` holds the build-time Python that produces committed artifacts — the filterbank
table in `src/core/features/filterbank_table.cpp` and the golden feature files under
`tests/data/features/` — by running madmom, the reference implementation, at a pinned
commit. Nothing in it is needed to build, test or run takt4, and CI never installs it.
It is needed when adding golden excerpts or bumping the pinned numpy/scipy/madmom:

```sh
python -m venv .venv
.venv\Scripts\activate                                     # . .venv/bin/activate elsewhere
pip install -r tools/requirements-build.txt
pip install --no-build-isolation -r tools/requirements.txt
python tools/make_golden.py path/to/track.flac --auto       # 10 s excerpt + madmom features
python tools/dump_filterbank.py                             # regenerate the table
```

Two steps because madmom builds from source and its `setup.py` imports numpy and
Cython. Full-length source tracks belong outside git; `references/` is ignored for
that purpose.

## Building

Requirements:

- CMake 3.28 or newer
- A C++20 compiler — Visual Studio 2022 or 2026, Apple Clang, or GCC
- A Rust toolchain, 1.92 or newer, on `PATH` (Slint is built from source through cargo),
  and `curl` on `PATH` (Slint's Skia renderer is not built from source: its bindings
  crate downloads a prebuilt Skia archive, 17–26 MB depending on the platform, with
  `curl` during the build). Only needed when the UI is built; see `TAKT4_BUILD_UI` below.
- Ninja on macOS and Linux
- Linux packages, Debian/Ubuntu names: `pkg-config libasound2-dev libjack-jackd2-dev
  libfontconfig-dev libfreetype-dev`. Everything else Slint needs on Linux (X11, xcb,
  xkbcommon, Wayland, EGL/GLX) is loaded at run time.

On Linux the binary links `libjack.so.0` directly, so it needs that library at run time
even when no JACK server is running (without one, JACK devices are simply not offered).
jackd2's `libjack-jackd2-0` puts it on the default library path. PipeWire's
`pipewire-jack` installs it under `/usr/lib/<triplet>/pipewire-0.3/jack/` instead, which
is reached by running takt4 through `pw-jack` or by enabling the `ld.so.conf.d` snippet
that package ships under `/usr/share/doc/pipewire/examples/`.

PortAudio, Ableton Link and the Steinberg ASIO SDK are in `third_party/` (see
[third_party/README.md](third_party/README.md)); the first two are submodules, so clone
with them:

```sh
git clone --recurse-submodules https://github.com/Fohdeesha/takt4.git
cd takt4
```

Everything else is fetched at configure time from pinned, hash-checked release
archives, so the first configure needs network access. So does the first build of the
UI: cargo fetches Slint's crate dependencies (checksummed by cargo) and the Skia
archive described above (which rust-skia's build script does not checksum).

Configure, build and test with the presets for your platform — `windows-msvc`, `macos`
or `linux`:

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

The build tree is `build/<preset>/`; the executable is `build/<preset>/bin/takt4`
(`bin/Release/takt4.exe` with Visual Studio). The `windows-msvc` preset names no
generator on purpose: CMake picks the newest Visual Studio it knows and finds, x64, so
the same preset serves a 2022 install and the 2026-only CI image.

CI (`.github/workflows/`) builds and tests the `*-core` presets on every push and the
full presets with the UI weekly, on demand, and whenever `src/ui/`, `cmake/` or the
CMake files change.

The UI is drawn by Slint's Skia renderer: Metal on macOS, OpenGL on Windows and Linux,
falling back to Skia's software rasteriser when no GPU context can be created. Setting
`SLINT_BACKEND=winit-skia-software` in the environment forces the software path, and
`SLINT_DEBUG_PERFORMANCE=refresh_lazy,console` makes takt4 report the surface it is
using on stderr. Skia is about 8 MB of the 18 MB Windows executable.

### Options

| Option | Default | Effect |
|---|---|---|
| `TAKT4_BUILD_UI` | `ON` | Build the Slint UI and the `takt4` executable. `OFF` builds the engine library, `takt4-cli` and the tests only, needs no Rust toolchain, and never fetches Slint. The `windows-core`, `macos-core` and `linux-core` presets set this. |
| `TAKT4_BUILD_TESTS` | `ON` when top-level | Build the Catch2 test suite. |
| `TAKT4_WARNINGS_AS_ERRORS` | `ON` | `/WX` or `-Werror` for takt4's own sources. Third-party code is compiled as system headers and is never subject to these flags. |

## Layout

```
src/core/     the engine — no UI dependency, must always build without Slint
src/core/audio/     devices, channel picking, resampling, hop accumulation
src/core/dsp/       real FFT (KissFFT)
src/core/features/  madmom-equivalent feature front end: STFT, filterbank, log, diff
src/core/io/        WAV and .npy readers/writers for tests and tools, not the audio path
src/core/rt/        real-time allocation guard, lock-free SPSC ring
src/cli/      takt4-cli, the development console; links takt4_core only
src/ui/       Slint markup and the C++ that binds it to the engine
src/main.cpp
tests/        Catch2; links takt4_core only
tests/data/   golden excerpts: audio plus madmom's features for it
tools/        Python that generates committed artifacts from madmom (see above)
third_party/  PortAudio, Ableton Link (+ Kohlhoff asio), Steinberg ASIO SDK
cmake/        dependency wiring, warning flags, ASIO SDK handling, the MSVC alloca shim
```

## License

GPLv3 — see [LICENSE](LICENSE).

| Component | Used for | License |
|---|---|---|
| [PortAudio](https://github.com/PortAudio/portaudio) | Audio input: ASIO, WASAPI, CoreAudio, ALSA, JACK | MIT |
| [r8brain-free-src](https://github.com/avaneev/r8brain-free-src) | Resampling the input to 22050 Hz | MIT |
| [KissFFT](https://github.com/mborgerding/kissfft) | The STFT behind the feature front end | BSD-3-Clause |
| [Ableton Link](https://github.com/Ableton/link) | Tempo sync | GPLv2 or later |
| [asio](https://github.com/chriskohlhoff/asio) (Kohlhoff, bundled by Link) | Networking for Link | Boost Software License |
| [Steinberg ASIO SDK](https://www.steinberg.net/asiosdk) | ASIO host API on Windows | GPLv3 (dual-licensed; see [third_party/README.md](third_party/README.md)) |
| [RTNeural](https://github.com/jatinchowdhury18/RTNeural) | Neural inference | BSD-3-Clause |
| [Eigen](https://eigen.tuxfamily.org) (bundled by RTNeural) | RTNeural's math backend | MPL-2.0 |
| [RtMidi](https://github.com/thestk/rtmidi) | MIDI clock and notes | MIT-style |
| [nlohmann/json](https://github.com/nlohmann/json) | Settings and presets | MIT |
| [Slint](https://slint.dev) | User interface | GPLv3 (triple-licensed) |
| [Skia](https://skia.org) (prebuilt by [rust-skia](https://github.com/rust-skia/rust-skia), pulled in by Slint) | UI rendering | BSD-3-Clause; the archive bundles libpng, zlib, libjpeg-turbo, expat, HarfBuzz, ICU and wuffs under their own permissive licenses |
| [Catch2](https://github.com/catchorg/Catch2) | Tests only, not shipped | Boost Software License |
