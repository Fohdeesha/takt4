# takt4

A desktop application that listens to one selectable channel of a multi-channel audio
interface, tracks beat, downbeat, tempo and meter in real time, and broadcasts that
timing over Ableton Link, OSC and MIDI clock — with a GUI-configurable rule engine for
firing events on the beat.

Aimed at live production in general: VJ software, lighting desks, media servers, DAWs,
generative visuals. Windows, macOS and Linux.

## Status

Early. The build system, dependencies and CI are in place; the application does not
track anything yet. `takt4 --version` prints what it was built with, and the window
opens.

## Building

Requirements:

- CMake 3.28 or newer
- A C++20 compiler — MSVC 2022, Apple Clang, or GCC
- A Rust toolchain, 1.92 or newer, on `PATH` (Slint is built from source through cargo).
  Only needed when the UI is built; see `TAKT4_BUILD_UI` below.
- Ninja on macOS and Linux
- Linux packages, Debian/Ubuntu names: `pkg-config libasound2-dev libfontconfig-dev`.
  Everything else Slint needs on Linux (X11, xcb, xkbcommon, Wayland, EGL/GLX) is loaded
  at run time.

PortAudio, Ableton Link and the Steinberg ASIO SDK are in `third_party/` (see
[third_party/README.md](third_party/README.md)); the first two are submodules, so clone
with them:

```sh
git clone --recurse-submodules https://github.com/Fohdeesha/takt4.git
cd takt4
```

Everything else is fetched at configure time from pinned, hash-checked release
archives; the first configure needs network access.

Configure, build and test with the presets for your platform — `windows-msvc`, `macos`
or `linux`:

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

The build tree is `build/<preset>/`; the executable is `build/<preset>/bin/takt4`
(`bin/Release/takt4.exe` with Visual Studio).

### Options

| Option | Default | Effect |
|---|---|---|
| `TAKT4_BUILD_UI` | `ON` | Build the Slint UI and the `takt4` executable. `OFF` builds the engine library and tests only, needs no Rust toolchain, and never fetches Slint. The `linux-core` preset sets this. |
| `TAKT4_BUILD_TESTS` | `ON` when top-level | Build the Catch2 test suite. |
| `TAKT4_WARNINGS_AS_ERRORS` | `ON` | `/WX` or `-Werror` for takt4's own sources. Third-party code is compiled as system headers and is never subject to these flags. |

## Layout

```
src/core/     the engine — no UI dependency, must always build without Slint
src/ui/       Slint markup and the C++ that binds it to the engine
src/main.cpp
tests/        Catch2; links takt4_core only
third_party/  PortAudio, Ableton Link (+ Kohlhoff asio), Steinberg ASIO SDK
cmake/        dependency wiring, warning flags, ASIO SDK handling
```

## License

GPLv3 — see [LICENSE](LICENSE).

| Component | Used for | License |
|---|---|---|
| [PortAudio](https://github.com/PortAudio/portaudio) | Audio input: ASIO, WASAPI, CoreAudio, ALSA | MIT |
| [Ableton Link](https://github.com/Ableton/link) | Tempo sync | GPLv2 or later |
| [asio](https://github.com/chriskohlhoff/asio) (Kohlhoff, bundled by Link) | Networking for Link | Boost Software License |
| [Steinberg ASIO SDK](https://www.steinberg.net/asiosdk) | ASIO host API on Windows | GPLv3 (dual-licensed; see [third_party/README.md](third_party/README.md)) |
| [RTNeural](https://github.com/jatinchowdhury18/RTNeural) | Neural inference | BSD-3-Clause |
| [Eigen](https://eigen.tuxfamily.org) (bundled by RTNeural) | RTNeural's math backend | MPL-2.0 |
| [RtMidi](https://github.com/thestk/rtmidi) | MIDI clock and notes | MIT-style |
| [nlohmann/json](https://github.com/nlohmann/json) | Settings and presets | MIT |
| [Slint](https://slint.dev) | User interface | GPLv3 (triple-licensed) |
| [Catch2](https://github.com/catchorg/Catch2) | Tests only, not shipped | Boost Software License |
