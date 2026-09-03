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

# Fails unless every one of the given definitions is in <target>'s <property>. PortAudio's
# and RtMidi's optional backends are switched on only when the library each needs is
# found; when it is not, the request is dropped without a word (a cmake_dependent_option
# is forced OFF), and the build would quietly ship without that backend.
function(takt4_require_definitions target property)
  get_target_property(defs ${target} ${property})
  foreach(def IN LISTS ARGN)
    if(NOT def IN_LIST defs)
      message(FATAL_ERROR
        "${target} was configured without ${def}: a backend takt4 needs was dropped. "
        "On Linux this usually means a development package is missing; see README.md."
      )
    endif()
  endforeach()
endfunction()

# The opposite: fails if any of the given definitions is in <target>'s <property>, for
# backends that were switched off on purpose and must not creep back in.
function(takt4_forbid_definitions target property)
  get_target_property(defs ${target} ${property})
  foreach(def IN LISTS ARGN)
    if(def IN_LIST defs)
      message(FATAL_ERROR "${target} was configured with ${def}, a backend takt4 leaves out on purpose.")
    endif()
  endforeach()
endfunction()

# ---------------------------------------------------------------------------------------
# PortAudio (submodule). Static. On Windows the host APIs are ASIO (through the vendored
# SDK) and WASAPI, the two HANDOFF §5.1 picks channels on; MME, DirectSound and WDM-KS
# would only list every interface three more times, and WDM-KS enumeration is what makes
# Pa_Initialize slow.
# On Linux the host APIs are ALSA and JACK, and nothing else: left to itself, PortAudio
# switches PulseAudio and sndio on or off depending on which -dev packages happen to be
# installed. JACK is linked directly (libjack.so.0, or PipeWire's replacement for it),
# so every Linux binary needs one of the two at run time; that was accepted so that JACK
# and PipeWire ports show up as ordinary devices. A server does not have to be running:
# PortAudio opens its client with JackNoStartServer, and without a server the host API
# is simply absent from the list.
# ---------------------------------------------------------------------------------------
block()
  if(WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/asiosdk.cmake")   # defines ASIO::host
    if(NOT TARGET ASIO::host)
      # Without the target, PortAudio downloads the SDK from Steinberg during configure.
      message(FATAL_ERROR "ASIO::host was not defined; PortAudio would fetch the SDK itself")
    endif()
    set(PA_USE_ASIO ON)
    set(PA_USE_WASAPI ON)
    set(PA_USE_WMME OFF)
    set(PA_USE_DS OFF)
    set(PA_USE_WDMKS OFF)
    set(PA_USE_WDMKS_DEVICE_INFO OFF)   # only WMME and DirectSound use it
  endif()
  if(LINUX)
    find_package(ALSA REQUIRED)   # libasound2-dev; used by PortAudio and RtMidi
    set(PA_USE_JACK ON)           # libjack-jackd2-dev; used by PortAudio and RtMidi
    set(PA_USE_PULSEAUDIO OFF)
    set(PA_USE_SNDIO OFF)
  else()
    set(PA_USE_JACK OFF)
  endif()
  set(PA_BUILD_SHARED_LIBS OFF)
  set(PA_BUILD_TESTS OFF)
  set(PA_BUILD_EXAMPLES OFF)
  set(PA_USE_OSS OFF)
  add_subdirectory("${TAKT4_THIRD_PARTY_DIR}/portaudio" EXCLUDE_FROM_ALL SYSTEM)
endblock()

if(WIN32)
  takt4_require_definitions(portaudio INTERFACE_COMPILE_DEFINITIONS PA_USE_ASIO=1 PA_USE_WASAPI=1)
  takt4_forbid_definitions(portaudio INTERFACE_COMPILE_DEFINITIONS PA_USE_WMME=1 PA_USE_DS=1 PA_USE_WDMKS=1)
elseif(APPLE)
  takt4_require_definitions(portaudio INTERFACE_COMPILE_DEFINITIONS PA_USE_COREAUDIO=1)
elseif(LINUX)
  takt4_require_definitions(portaudio INTERFACE_COMPILE_DEFINITIONS PA_USE_ALSA=1 PA_USE_JACK=1)
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

# One LSTMLayerT<float, 150, 150> holds a fixed 600 x 301 matrix — 722 KB, well past
# Eigen's default 128 KB cap on a fixed-size object, which BeatNet+'s four stacked
# layers would otherwise fail to compile against. Zero means "no cap" (DenseStorage.h),
# and it also makes Eigen heap-allocate any dynamic temporary rather than alloca it:
# takt4 keeps the whole network on the heap behind BeatModel, and a stray temporary
# should fail the [rt] allocation test loudly rather than eat the audio thread's stack.
target_compile_definitions(RTNeural PUBLIC EIGEN_STACK_ALLOCATION_LIMIT=0)

# ---------------------------------------------------------------------------------------
# RtMidi 6.0.0. Static. Its JACK API follows PortAudio's: on for Linux, off elsewhere,
# where RtMidi would otherwise switch it on by itself on any machine with a libjack.
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
  if(LINUX)
    set(RTMIDI_API_JACK ON)   # RtMidi fails the configure itself if libjack is missing
  else()
    set(RTMIDI_API_JACK OFF)
  endif()
  set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi_uninstall")   # PortAudio already owns "uninstall"
  FetchContent_MakeAvailable(rtmidi)
endblock()
set(TAKT4_RTMIDI_VERSION "6.0.0")

# RtMidi's API macros are PRIVATE to its target, hence COMPILE_DEFINITIONS.
if(WIN32)
  takt4_require_definitions(rtmidi COMPILE_DEFINITIONS __WINDOWS_MM__)
elseif(APPLE)
  takt4_require_definitions(rtmidi COMPILE_DEFINITIONS __MACOSX_CORE__)
elseif(LINUX)
  takt4_require_definitions(rtmidi COMPILE_DEFINITIONS __LINUX_ALSA__ __UNIX_JACK__)
endif()

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
# r8brain-free-src 7.5 (header-only since 7.0; the default Ooura FFT needs no extra
# source file). The archive carries no CMake project, so this declares the target.
# Its version string is read from the header (R8B_VERSION) rather than pinned here.
# ---------------------------------------------------------------------------------------
FetchContent_Declare(r8brain
  URL "https://github.com/avaneev/r8brain-free-src/archive/refs/tags/7.5.tar.gz"
  URL_HASH SHA256=d3350b3045139e2928fdef3eab7c0755dcc69997af4a8957aea890a8a87d2043
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(r8brain)
if(NOT EXISTS "${r8brain_SOURCE_DIR}/CDSPResampler.h")
  message(FATAL_ERROR "r8brain archive layout changed: CDSPResampler.h not found in ${r8brain_SOURCE_DIR}")
endif()
add_library(r8brain INTERFACE)
target_include_directories(r8brain SYSTEM INTERFACE "${r8brain_SOURCE_DIR}")

# ---------------------------------------------------------------------------------------
# KissFFT 131.2.0 (HANDOFF §5.2: the STFT). Static, double precision.
#
# The 1764-point frame factors as 2²·3²·7², and for the 7s KissFFT's generic butterfly
# takes a scratch buffer per call, from malloc unless KISS_FFT_USE_ALLOCA puts it on the
# stack — which it must, because the transform runs on the audio thread (§4.2). With
# that define _kiss_fft_guts.h includes <alloca.h>, a header MSVC does not have (its
# alloca is in <malloc.h>), so KissFFT's own compilation gets cmake/shims/msvc-alloca
# on its include path. KissFFT itself is not patched. Its other case for a scratch
# buffer, an in-place transform, does not arise: the wrapper in src/core/dsp always
# transforms out of place.
#
# KissFFT's CMakeLists reads its version from its Makefile, and the Makefile in the
# 131.2.0 archive still says 131.1.0: upstream tagged the release without bumping it
# (master is the same as of 2026-09). So the tag is what build_info reports, and the
# stale self-reported version is pinned alongside it so that a bumped archive that
# says something else is noticed rather than silently reported as 131.2.0.
# ---------------------------------------------------------------------------------------
FetchContent_Declare(kissfft
  URL "https://github.com/mborgerding/kissfft/archive/refs/tags/131.2.0.tar.gz"
  URL_HASH SHA256=205a8f6a448ef12b091f8ac6a514b5091bb5f6b0b543431ed75f673116cf5cbf
  SYSTEM
  EXCLUDE_FROM_ALL
)
block()
  set(KISSFFT_DATATYPE "double")
  set(KISSFFT_STATIC ON)
  set(KISSFFT_USE_ALLOCA ON)
  set(KISSFFT_OPENMP OFF)
  set(KISSFFT_PKGCONFIG OFF)
  set(KISSFFT_TEST OFF)
  set(KISSFFT_TOOLS OFF)
  FetchContent_MakeAvailable(kissfft)
endblock()
set(TAKT4_KISSFFT_VERSION "131.2.0")   # the tag
set(kissfft_self_reported_version "131.1.0")   # what its Makefile says; see above

takt4_require_definitions(kissfft INTERFACE_COMPILE_DEFINITIONS kiss_fft_scalar=double KISS_FFT_USE_ALLOCA)
takt4_forbid_definitions(kissfft INTERFACE_COMPILE_DEFINITIONS KISS_FFT_SHARED)

FetchContent_GetProperties(kissfft SOURCE_DIR kissfft_SOURCE_DIR)
file(READ "${kissfft_SOURCE_DIR}/Makefile" kissfft_makefile)
string(REGEX MATCH "KFVER_MAJOR = ([0-9]+)\n.*KFVER_MINOR = ([0-9]+)\n.*KFVER_PATCH = ([0-9]+)\n" _ "${kissfft_makefile}")
if(NOT "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}" STREQUAL kissfft_self_reported_version)
  message(FATAL_ERROR
    "KissFFT's Makefile says ${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}, not the "
    "${kissfft_self_reported_version} expected for ${TAKT4_KISSFFT_VERSION}. Re-pin both in cmake/deps.cmake."
  )
endif()
unset(kissfft_makefile)
unset(kissfft_self_reported_version)

if(MSVC)
  target_include_directories(kissfft PRIVATE "${CMAKE_CURRENT_LIST_DIR}/shims/msvc-alloca")
endif()

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
    # Skia rather than FemtoVG (HANDOFF §2): Metal on macOS, OpenGL elsewhere. Skia itself
    # is not compiled here: skia-bindings' build script downloads rust-skia's prebuilt
    # archive for the target (17-26 MB, with curl, not checksummed) on every clean build.
    set(SLINT_FEATURE_RENDERER_SKIA ON)
    set(SLINT_FEATURE_RENDERER_FEMTOVG OFF)
    if(WIN32)
      # Without this Skia draws on the CPU on Windows: its default surface there is
      # softbuffer, and Direct3D is only reachable from Slint's Rust API. The feature is
      # global to the build and would replace Metal on macOS, hence the guard. A GL
      # context that cannot be created still falls back to the software surface.
      set(SLINT_FEATURE_RENDERER_SKIA_OPENGL ON)
    endif()
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
  # `cargo tree --target <triple>` declare plus what skia-bindings' build script emits
  # for the platform (rust-skia's build_support/platform/*.rs). Re-check them when
  # bumping Slint.
  if(WIN32)
    # Skia's own archives (prebuilt, fetched by skia-bindings' build script) are handed to
    # cargo as plain `-l skia` on Windows rather than `-l static=skia` (rust-skia PR #354),
    # so rustc leaves them out of the staticlib. Ask for them as static libraries on the
    # final rustc invocation instead: cargo passes skia-bindings' link-search path to that
    # invocation, and rustc bundles static libraries into a staticlib by default. This is
    # what rust-skia itself does on every other platform. The names are rust-skia's
    # binaries_config.rs list for the textlayout feature set Slint enables.
    corrosion_add_target_local_rustflags(slint_cpp
      -lstatic=skia -lstatic=skia-bindings
      -lstatic=skparagraph -lstatic=skshaper -lstatic=skunicode_core -lstatic=skunicode_icu
    )
    # opengl32: glutin's WGL bindings and Skia's GL backend. imm32: winit's IME support,
    # declared through windows-targets 0.52, which resolves against its bundled
    # windows.0.52.0.lib; of the DLLs that import library covers, imm32 is the only one
    # outside CMake's default link set. shlwapi: the webbrowser crate. usp10, fontsub,
    # d3d12, dxgi, d3dcompiler: rust-skia's platform/windows.rs list for the gl and d3d
    # features, minus the libraries CMake links by default (user32 gdi32 ole32 advapi32).
    set_property(TARGET slint_cpp-static APPEND PROPERTY INTERFACE_LINK_LIBRARIES
      opengl32 imm32 shlwapi usp10 fontsub d3d12 dxgi d3dcompiler
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
      # Skia's Metal backend (rust-skia platform/macos.rs: Metal, MetalKit, Foundation;
      # ApplicationServices and OpenGL are already above).
      "-framework Metal"
      "-framework MetalKit"
    )
  elseif(LINUX)
    # System font enumeration goes through libfontconfig, and the prebuilt Skia archive
    # renders glyphs with the system FreeType (rust-skia platform/linux.rs links both).
    # Everything else Slint touches on Linux (X11, xcb, xkbcommon, Wayland, EGL, GLX) is
    # dlopen'ed at run time.
    #
    # GLOBAL matters: a subdirectory only sees the imported targets its parent had when
    # add_subdirectory ran, and Slint's directory was created above, before this call.
    # The imported targets are resolved from slint_cpp-static's directory when takt4 is
    # generated, so without GLOBAL they are "not found" there.
    find_package(Fontconfig REQUIRED GLOBAL)   # libfontconfig-dev
    find_package(Freetype REQUIRED GLOBAL)     # libfreetype-dev
    set_property(TARGET slint_cpp-static APPEND PROPERTY INTERFACE_LINK_LIBRARIES
      Fontconfig::Fontconfig Freetype::Freetype
    )
  endif()
endif()
