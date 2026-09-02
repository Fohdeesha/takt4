# Vendored dependencies

Everything in this directory needs build configuration that `FetchContent` cannot
express cleanly, so it is vendored — as a git submodule where upstream is a git
repository, as a plain copy otherwise. Dependencies that *can* be fetched at
configure time live in `cmake/deps.cmake` instead.

| Directory | What | Version | License | Notes |
|---|---|---|---|---|
| `portaudio/` | PortAudio | master `bc2f865c` (2026-08-27) | MIT | The last tagged release, v19.7.0, is from 2021 and predates the CMake build that supports `PA_USE_ASIO`. Pinned to a master commit instead; bump deliberately. |
| `link/` | Ableton Link | `Link-4.0` (2026-04-14) | GPLv2-or-later, used as GPLv3 | Header-only. Its `modules/asio-standalone` submodule is **Chris Kohlhoff's asio networking library** (Boost Software License), pinned by Link at `asio-1-36-0`. It has nothing to do with Steinberg ASIO — see below. |
| `asiosdk/` | Steinberg ASIO SDK | 2.3.4 (© 2025) | Dual: proprietary **or GPLv3** — this project uses it under GPLv3 | Feeds PortAudio's ASIO host API on Windows. Not a git repository upstream; copied from the official SDK package. |

## Steinberg ASIO SDK — what is vendored

The SDK package from <https://www.steinberg.net/asiosdk> contains more than the
host-side sources this project compiles. Vendored, byte-identical to the package:

- `LICENSE.txt`, `README.md`, `changes.txt` — the dual-license terms and SDK notes
- `Steinberg ASIO SDK 2.3.pdf` — the interface specification
- `common/` — `asio.h`, `iasiodrv.h`, `asio.cpp`, and supporting sources
- `host/` — `asiodrivers.*`, `ASIOConvertSamples.*`, `host/pc/asiolist.*`

Omitted from the package, deliberately:

- `driver/` and `host/sample/` — driver-side and host sample projects, not used
- `asio/` — Visual Studio 6 workspace files
- `Steinberg ASIO Logo Artwork/` and `Steinberg ASIO Usage Guidelines.pdf` — trademark
  material. This project does not display the ASIO logo. The name "ASIO" appears only
  where PortAudio reports it as a host API, which the SDK README permits.
- `Steinberg ASIO Licensing Agreement.pdf` — the proprietary alternative, not chosen

`host/pc/asiolist.cpp` frees an array with `delete` instead of `delete[]`. PortAudio's
own `FindASIO.cmake` patches this at configure time; `cmake/asiosdk.cmake` applies the
same patch into the build directory so the vendored tree stays pristine.

## Two unrelated things called ASIO

- **Kohlhoff asio** (`link/modules/asio-standalone`): C++ networking, `io_context`,
  UDP sockets, multicast. Link uses it for peer discovery. Namespaced by Link as
  `link_asio_1_36_0`.
- **Steinberg ASIO** (`asiosdk/`): the Windows low-latency audio driver interface,
  `ASIOStart()`, `ASIOCreateBuffers()`, `bufferSwitch()`.

They are both needed and must never be confused for one another.
