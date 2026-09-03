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
ulp.

The BeatNet+ neural network runs on top of that: convolution block, a dense layer, four
stacked LSTMs and a softmax over beat / downbeat / non-beat, 50 times a second, through
RTNeural. All three published weight sets are converted into `assets/weights/` and
checked against PyTorch on the same eighteen excerpts (`tests/data/model/`), where the
largest difference in the probabilities is 3.6e-6. Live, the audio callback only hands
hops to a lock-free ring and a worker thread runs the front end and the model; over
those excerpts the worker averages 0.086 ms per hop and has never taken more than
0.31 ms, out of the 20 ms of audio each hop stands for.

Those probabilities become beats through BeatNet+'s two-stage particle filter cascade.
madmom's bar-pointer state space and its transition models are precomputed into
`assets/statespace/default.bin` — 40 KB covering 55 to 215 BPM and 2 to 4 beats to the
bar — so the C++ implements only the runtime loop. It is deterministic: the generator is
specified rather than inherited, and `src/core/tracking/` reproduces
`tools/pf_reference.py` frame for frame on all eighteen excerpts. That restatement in
turn has to track the real BeatNet+ filter as closely as it tracks itself across six
seeds before anything is committed; see
[tests/data/tracking/README.md](tests/data/tracking/README.md), which also records what
that catches and what it cannot.

On top sits the tempo state machine: octave folding into a range you set, a lock that
needs sustained agreement and sustained disagreement to change, a confidence gate that
holds the last good tempo rather than publishing a wrong one, a latency offset on every
beat, and the meter taken from the filter rather than assumed. Tempo is refined from the
spacing of the beats themselves, because the state space's whole-frame intervals are 5
BPM apart at 130 and nothing inside the filter can do better.

The three transports are driven from it: Ableton Link (tempo, and phase with the
detected meter as the quantum, timed through Link's own regression on the audio thread's
sample counter), a generic OSC namespace on any number of targets, and MIDI beat clock
at 24 PPQN.

`takt4 --version` prints what it was built with, and the window opens; the UI does not
show any of this yet.

### Development console

`takt4-cli` is built alongside the application (in `bin/` next to it) but never
packaged. It exercises the engine without the UI:

```sh
takt4-cli devices                       # host APIs, then every input device and its channels
takt4-cli meter --device 1 --channel 7  # open input 7 of device 1, print RMS/peak at 10 Hz
takt4-cli meter --device 1 --channels 7,8
takt4-cli meter --device 1 --all        # every channel of the device, unresampled
takt4-cli features in.wav out.npy --compare golden.npy   # feature front end on a file
takt4-cli beats in.wav                                   # front end + model on a file
takt4-cli beats --device 1 --channel 7                   # ... and on a live input
takt4-cli track in.wav --bpm 80-160                      # the whole chain, over a file
takt4-cli track --device 1 --channel 7 --link --osc 192.168.1.40:7000 --midi-clock "MOTU"
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

`beats` adds the neural network and prints P(beat), P(downbeat) and P(non-beat) per
20 ms frame, either over a file or from a live input — the device options are the
meter's. `--weights` picks the weight set: `generic` (the default), `generic-main` for
percussion-heavy material, `af-non-percussive` for ambient and classical, or a path to
a `.bin` of your own. A run ends with the mean and the worst time one hop took on the
worker thread, out of the 20 ms of audio it stands for.

`track` runs everything: the front end, the model, the particle filter and the tempo
state machine, and drives the outputs. It prints a line per beat with the tempo, the
position in the bar, the meter, whether the tempo is locked and how confident the
tracker is, and a status line every two seconds in between.

| Option | |
|---|---|
| `--bpm LO-HI` | the octave-fold window, default `70-140`. An estimate outside it is halved or doubled into it, which is what stops a house set reading 170. |
| `--confidence T` | below this the last good tempo is held and the line says so; default 0.15 |
| `--latency MS` | added to every beat's timestamp and to what the transports are told; negative fires early, which is the useful direction |
| `--seed N` | the particle filter's seed. The same seed and the same audio give the same beats, every time and on every platform. |
| `--link` | join the Ableton Link network as tempo master |
| `--osc HOST:PORT` | send the generic namespace there; repeat for more targets |
| `--osc-prefix /NAME` | that namespace's prefix, default `/takt4` |
| `--midi-clock PORT` | 24 PPQN to a MIDI output port, named by any part of its name or by its index. A wrong name lists the ports that are there. |

The OSC namespace is `/takt4/bpm`, `/takt4/beat`, `/takt4/beat/bar`, `/takt4/downbeat`,
`/takt4/confidence`, `/takt4/locked`, `/takt4/meter` and `/takt4/resync`. The state
addresses repeat on every beat, so anything that starts late is right again within a
beat.

The outputs need a live input: a file is worked through as fast as it reads, so its
beats do not happen in real time and there is no host clock to align a transport to.
Over a file `track` prints the beats and nothing else.

### Python tooling

`tools/` holds the build-time Python that produces committed artifacts — the filterbank
table in `src/core/features/filterbank_table.cpp`, the golden feature files under
`tests/data/features/`, the weight blobs in `assets/weights/`, the state space in
`assets/statespace/`, and the reference activations and tracker traces under
`tests/data/model/` and `tests/data/tracking/` — by running madmom and PyTorch, the
reference implementations, at pinned versions. Nothing in it is needed to build, test or
run takt4, and CI never installs it. It is needed when adding golden excerpts or bumping
the pinned numpy/scipy/madmom:

```sh
python -m venv .venv
.venv\Scripts\activate                                     # . .venv/bin/activate elsewhere
pip install -r tools/requirements-build.txt
pip install --no-build-isolation -r tools/requirements.txt
python tools/make_golden.py path/to/track.flac --auto       # 10 s excerpt + madmom features
python tools/dump_filterbank.py                             # regenerate the table
python tools/dump_statespace.py                             # regenerate the state space
python tools/pf_reference.py                                # regenerate the tracker traces
```

`pf_reference.py` also needs a BeatNet+ checkout, because it refuses to write anything
until this project's restatement of the particle filter has been held against the real
one.

Two steps because madmom builds from source and its `setup.py` imports numpy and
Cython. Full-length source tracks belong outside git; `references/` is ignored for
that purpose.

The two tools that need PyTorch are separate, because it is a 250 MB install nothing
else here wants, and they also need BeatNet+'s published weight files, which are not
vendored (see [tests/data/model/README.md](tests/data/model/README.md)):

```sh
pip install -r tools/requirements-torch.txt
python tools/convert_weights.py references/beatnet-plus/src/BeatNetPlus/models
python tools/model_reference.py
```

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
src/core/model/     BeatNet+ through RTNeural: weight loading, the network, the worker
src/core/output/    Ableton Link, the OSC encoder and sender, MIDI clock
src/core/rt/        real-time allocation guard, lock-free SPSC ring
src/core/tracking/  the state space, the particle filter, the tempo state machine
src/cli/      takt4-cli, the development console; links takt4_core only
src/ui/       Slint markup and the C++ that binds it to the engine
src/main.cpp
assets/weights/    the three BeatNet+ weight sets, converted (tools/convert_weights.py)
assets/statespace/ madmom's bar-pointer state space, precomputed (tools/dump_statespace.py)
tests/        Catch2; links takt4_core only
tests/data/   golden excerpts: audio, madmom's features for it, PyTorch's activations
tools/        Python that generates committed artifacts from madmom and torch (above)
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
| [BeatNet+](https://github.com/mjhydri/BeatNet-Plus) weights (`assets/weights/`) | Beat, downbeat and meter detection | **None stated upstream** |
| [Eigen](https://eigen.tuxfamily.org) (bundled by RTNeural) | RTNeural's math backend | MPL-2.0 |
| [RtMidi](https://github.com/thestk/rtmidi) | MIDI clock and notes | MIT-style |
| [madmom](https://github.com/CPJKU/madmom) | Build-time only, never linked or shipped: `tools/` runs it to compute the filterbank table and the state space blob, and to produce the golden features the C++ front end is checked against | 2-clause BSD for its source, which is all that is used. Its *pretrained data and model files* are CC BY-NC-SA 4.0; takt4 loads none of them — every table here is computed from configuration, not copied from a `.npy` or a pickled model. |
| [nlohmann/json](https://github.com/nlohmann/json) | Settings and presets | MIT |
| [Slint](https://slint.dev) | User interface | GPLv3 (triple-licensed) |
| [Skia](https://skia.org) (prebuilt by [rust-skia](https://github.com/rust-skia/rust-skia), pulled in by Slint) | UI rendering | BSD-3-Clause; the archive bundles libpng, zlib, libjpeg-turbo, expat, HarfBuzz, ICU and wuffs under their own permissive licenses |
| [Catch2](https://github.com/catchorg/Catch2) | Tests only, not shipped | Boost Software License |
