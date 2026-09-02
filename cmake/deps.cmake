# Third-party dependencies.
#
# Vendored ones live in third_party/ (see third_party/README.md for why). The rest are
# fetched at configure time from pinned release archives with a content hash, so a
# configure is reproducible and cannot silently pick up a newer upstream.
#
# Each dependency is configured inside a block() so the variables that steer its
# options — and the C++ standard Link's config file sets — do not leak into ours.
# All of their include directories are SYSTEM, so our warning flags stay ours.

include(FetchContent)

set(TAKT4_THIRD_PARTY_DIR "${PROJECT_SOURCE_DIR}/third_party")

foreach(marker IN ITEMS
  portaudio/CMakeLists.txt
  link/AbletonLinkConfig.cmake
  link/modules/asio-standalone/asio/include/asio.hpp
)
  if(NOT EXISTS "${TAKT4_THIRD_PARTY_DIR}/${marker}")
    message(FATAL_ERROR
      "third_party/${marker} is missing. The submodules are not checked out; run:\n"
      "  git submodule update --init --recursive"
    )
  endif()
endforeach()

# ---------------------------------------------------------------------------------------
# PortAudio (submodule). Static. On Windows, ASIO is enabled through the vendored SDK.
# On Linux the host API set is pinned to ALSA: PortAudio otherwise switches PulseAudio,
# sndio and JACK on or off depending on which -dev packages happen to be installed, and
# PortAudio and RtMidi both silently drop ALSA when its headers are missing. JACK is
# left out on purpose for now: PortAudio links libjack directly, which would make every
# Linux binary depend on it. Revisit when packaging for Linux.
# ---------------------------------------------------------------------------------------
block()
  if(WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/asiosdk.cmake")   # defines ASIO::host
    set(PA_USE_ASIO ON)
  endif()
  if(LINUX)
    find_package(ALSA REQUIRED)   # libasound2-dev; used by PortAudio and RtMidi
    set(PA_USE_PULSEAUDIO OFF)
    set(PA_USE_SNDIO OFF)
  endif()
  set(PA_BUILD_SHARED_LIBS OFF)
  set(PA_BUILD_TESTS OFF)
  set(PA_BUILD_EXAMPLES OFF)
  set(PA_USE_JACK OFF)
  set(PA_USE_OSS OFF)
  add_subdirectory("${TAKT4_THIRD_PARTY_DIR}/portaudio" EXCLUDE_FROM_ALL SYSTEM)
endblock()

if(WIN32 AND NOT TARGET ASIO::host)
  message(FATAL_ERROR "ASIO::host was not defined; PortAudio would build without ASIO")
endif()

# ---------------------------------------------------------------------------------------
# Ableton Link (submodule, header-only) plus the Kohlhoff asio it bundles.
# AbletonLinkConfig.cmake sets CMAKE_CXX_STANDARD 17; the block() keeps that from
# overriding our C++20.
# ---------------------------------------------------------------------------------------
block()
  include("${TAKT4_THIRD_PARTY_DIR}/link/AbletonLinkConfig.cmake")   # defines Ableton::Link
endblock()

# Tag of the third_party/link submodule. Keep in step when bumping it.
set(TAKT4_LINK_VERSION "4.0")

if(WIN32)
  # ThreadFactory.hpp calls AvSetMmThreadCharacteristicsW without a #pragma comment(lib).
  set_property(TARGET Ableton::Link APPEND PROPERTY INTERFACE_LINK_LIBRARIES avrt)
endif()

# ---------------------------------------------------------------------------------------
# RTNeural. No releases are tagged upstream, so a commit is pinned. The default Eigen
# backend is vendored inside the archive; xsimd (a submodule, not in the archive) is not
# needed for it.
# ---------------------------------------------------------------------------------------
set(TAKT4_RTNEURAL_REV "95c3c0f987a6fe903e7eec71e797405dbed7caf7")   # 2026-08-20

FetchContent_Declare(RTNeural
  URL "https://github.com/jatinchowdhury18/RTNeural/archive/${TAKT4_RTNEURAL_REV}.tar.gz"
  URL_HASH SHA256=a602b67b6c33c4f8e7e703e9aec100c26761af017f66c023a01104eb055947e1
  SYSTEM
  EXCLUDE_FROM_ALL
)
block()
  set(RTNEURAL_EIGEN ON)
  set(BUILD_TESTS OFF)      # RTNeural's option names are unprefixed
  set(BUILD_BENCH OFF)
  set(BUILD_EXAMPLES OFF)
  FetchContent_MakeAvailable(RTNeural)
endblock()
# RTNeural warns on every inclusion unless this is defined. 16 is its own default.
target_compile_definitions(RTNeural PUBLIC RTNEURAL_DEFAULT_ALIGNMENT=16)

# ---------------------------------------------------------------------------------------
# RtMidi 6.0.0. Static. JACK off for the same reason as PortAudio.
# ---------------------------------------------------------------------------------------
FetchContent_Declare(rtmidi
  URL "https://github.com/thestk/rtmidi/archive/refs/tags/6.0.0.tar.gz"
  URL_HASH SHA256=ef7bcda27fee6936b651c29ebe9544c74959d0b1583b716ce80a1c6fea7617f0
  SYSTEM
  EXCLUDE_FROM_ALL
)
block()
  set(RTMIDI_BUILD_STATIC_LIBS ON)
  set(RTMIDI_BUILD_TESTING OFF)
  set(RTMIDI_API_JACK OFF)
  set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall")   # PortAudio already owns "uninstall"
  FetchContent_MakeAvailable(rtmidi)
endblock()
set(TAKT4_RTMIDI_VERSION "6.0.0")

if(WIN32)
  # RtMidi compiles with RTMIDI_EXPORT (__declspec(dllexport) on its classes) even when
  # built static, so every executable that links it would carry an export table of RtMidi
  # symbols and a stray .exp/.lib pair. Take the definition back out.
  get_target_property(rtmidi_defs rtmidi COMPILE_DEFINITIONS)
  if(NOT rtmidi_defs)
    message(FATAL_ERROR "rtmidi has no COMPILE_DEFINITIONS; expected RTMIDI_EXPORT among them")
  endif()
  list(REMOVE_ITEM rtmidi_defs RTMIDI_EXPORT)
  set_target_properties(rtmidi PROPERTIES COMPILE_DEFINITIONS "${rtmidi_defs}")
  unset(rtmidi_defs)
endif()

# ---------------------------------------------------------------------------------------
# nlohmann/json 3.12.0 (release archive: header plus CMake config only, not the repo).
#
# RTNeural's model loader bundles its own copy (3.11.1) and includes it by relative
# path. The two must not meet in one translation unit, or the header's version check
# fires. Rule: a TU includes either <RTNeural/RTNeural.h> or <nlohmann/json.hpp>.
# ---------------------------------------------------------------------------------------
FetchContent_Declare(nlohmann_json
  URL "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz"
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
  SYSTEM
  EXCLUDE_FROM_ALL
)
block()
  set(JSON_BuildTests OFF)
  set(JSON_Install OFF)
  FetchContent_MakeAvailable(nlohmann_json)
endblock()

# ---------------------------------------------------------------------------------------
# Catch2 v3 (tests only).
# ---------------------------------------------------------------------------------------
if(TAKT4_BUILD_TESTS)
  FetchContent_Declare(Catch2
    URL "https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz"
    URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
    SYSTEM
    EXCLUDE_FROM_ALL
  )
  block()
    set(CATCH_INSTALL_DOCS OFF)
    set(CATCH_INSTALL_EXTRAS OFF)
    FetchContent_MakeAvailable(Catch2)
  endblock()
  # catch_discover_tests() lives in Catch2's extras directory.
  FetchContent_GetProperties(Catch2 SOURCE_DIR catch2_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# ---------------------------------------------------------------------------------------
# Slint 1.17.1 (UI only). Built from source through Corrosion and cargo, so a Rust
# toolchain (1.92 or newer) must be on PATH. Linked statically: takt4 is one executable
# and this removes any question of where a shared slint_cpp library has to live.
# The interpreter is not built; .slint files are compiled ahead of time.
# ---------------------------------------------------------------------------------------
if(TAKT4_BUILD_UI)
  FetchContent_Declare(Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG v1.17.1
    GIT_SHALLOW ON
    SOURCE_SUBDIR api/cpp
    SYSTEM
  )
  block()
    # BUILD_SHARED_LIBS is OFF at top level; Slint reads it and builds slint_cpp-static.
    set(SLINT_FEATURE_INTERPRETER OFF)
    set(SLINT_STYLE "fluent")
    FetchContent_MakeAvailable(Slint)
  endblock()
  set(TAKT4_SLINT_VERSION "1.17.1")

  if(NOT TARGET slint_cpp-static)
    message(FATAL_ERROR "Slint did not produce slint_cpp-static; a static build was expected")
  endif()

  # slint_cpp is a Rust staticlib. Corrosion attaches the system libraries the Rust
  # standard library needs, but not the ones Slint's crates request with #[link] or from
  # their build scripts; those only surface as unresolved symbols when takt4 is linked.
  # The lists below are for slint-cpp 1.17.1 with the features Slint's CMake passes, minus
  # what Corrosion and CMake link anyway: on Windows, what rustc reports when the cargo
  # command from the cargo-build_slint_cpp build rule is re-run with
  # `--print native-static-libs` appended; on the other two, what the crates in
  # `cargo tree --target <triple>` declare. Re-check them when bumping Slint.
  if(WIN32)
    # opengl32: glutin's WGL bindings. imm32: winit's IME support, declared through
    # windows-targets 0.52, which resolves against its bundled windows.0.52.0.lib; of the
    # DLLs that import library covers, imm32 is the only one outside CMake's default link
    # set. shlwapi: the webbrowser crate.
    set_property(TARGET slint_cpp-static APPEND PROPERTY INTERFACE_LINK_LIBRARIES
      opengl32 imm32 shlwapi
    )
    if(MSVC)
      # Slint's headers declare the runtime's entry points __declspec(dllimport) whether or
      # not it was built as a DLL (slint_config.h). Against the static library the linker
      # resolves them anyway and says so once per symbol: LNK4217 or, without the calling
      # function's name, LNK4049. Both describe exactly this situation, so silence them.
      set_property(TARGET slint_cpp-static APPEND PROPERTY INTERFACE_LINK_OPTIONS
        /ignore:4217 /ignore:4049
      )
    endif()
  elseif(APPLE)
    set_property(TARGET slint_cpp-static APPEND PROPERTY INTERFACE_LINK_LIBRARIES
      objc
      "-framework AppKit"
      "-framework Foundation"
      "-framework CoreFoundation"
      "-framework CoreGraphics"
      "-framework CoreText"
      "-framework CoreVideo"
      "-framework CoreImage"
      "-framework CoreData"
      "-framework CloudKit"
      "-framework QuartzCore"
      "-framework OpenGL"
      "-framework CoreServices"
      "-framework ApplicationServices"
      "-framework Carbon"
      "-framework Accessibility"
    )
  elseif(LINUX)
    # System font enumeration goes through libfontconfig. Everything else Slint touches on
    # Linux (X11, xcb, xkbcommon, Wayland, EGL, GLX) is dlopen'ed at run time.
    find_package(Fontconfig REQUIRED)   # libfontconfig-dev
    set_property(TARGET slint_cpp-static APPEND PROPERTY INTERFACE_LINK_LIBRARIES
      Fontconfig::Fontconfig
    )
  endif()
endif()
