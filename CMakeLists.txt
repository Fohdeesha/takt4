cmake_minimum_required(VERSION 3.28)

project(takt4
  VERSION 1.2.1
  DESCRIPTION "Real-time beat, downbeat and tempo tracker driving Ableton Link, OSC and MIDI"
  HOMEPAGE_URL "https://github.com/Fohdeesha/takt4"
  LANGUAGES C CXX
)

# Slint's C++ API requires C++20. Nothing else here needs more than C++17.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Third-party sub-builds declare their own option()s. These two policies make them
# honour the plain variables set for them in cmake/deps.cmake instead of the cache.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
# And this one makes them take the runtime library chosen below, which a sub-build that asks
# for an older CMake would otherwise ignore.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# takt4 is one statically linked executable. This is a plain variable, not an option(),
# on purpose: RtMidi declares option(BUILD_SHARED_LIBS ... ON), which would otherwise
# land in the cache and turn every later dependency (Catch2, for one) into a DLL.
set(BUILD_SHARED_LIBS OFF)

# Keep build products in one predictable place regardless of which subdirectory made them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TAKT4_BUILD_UI "Build the Slint user interface and the takt4 executable" ON)
option(TAKT4_BUILD_TESTS "Build the test suite" "${PROJECT_IS_TOP_LEVEL}")
option(TAKT4_WARNINGS_AS_ERRORS "Treat warnings in takt4's own sources as errors" ON)

# A release build that keeps its symbols.
#
# takt4 crashed on a rig on 2026-09-16 and left a minidump, and the dump named the faulting
# instruction as `takt4.exe+0xf80723` and could say no more, because the shipped build had no
# PDB to name it with. The optimisation is unchanged — `/Zi` only writes the symbols to a
# file beside the exe, and `/OPT:REF /OPT:ICF` put back the two folding passes that `/DEBUG`
# would otherwise turn off. Nothing about the binary changes; it just becomes readable when
# it falls over.
if(MSVC)
  add_compile_options($<$<CONFIG:Release>:/Zi>)
  add_link_options($<$<CONFIG:Release>:/DEBUG>
                   $<$<CONFIG:Release>:/OPT:REF>
                   $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# **The Visual C++ runtime is a DLL, and an older copy of it must not crash takt4.**
#
# takt4.exe imports MSVCP140.dll, VCRUNTIME140.dll and VCRUNTIME140_1.dll, which come with the
# Visual C++ redistributable (the audit's H17). Linking the runtime statically instead was
# tried on 2026-09-23 and cannot be done: Slint draws with Skia, which rust-skia ships as a
# prebuilt archive, and it publishes no static-runtime build of the one Slint needs (the
# download for `...-d3d-gl-jpegd-jpege-pdf-textlayout-static` is a 404) — so it would mean
# building Skia from source on every clean build. The README says what to install instead.
#
# What can be fixed is the other half of H17: from Visual Studio 2022 17.10, std::mutex has a
# constexpr constructor that an *older* msvcp140.dll on the machine does not understand, and a
# program built that way crashes on its first lock there. Microsoft's answer is this macro,
# for everything compiled here; Skia's prebuilt archive is out of reach.
if(MSVC)
  add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# **Every configuration links the release runtime, Debug included.**
#
# Rust links the release C runtime whatever the profile, and Skia's prebuilt archive is built
# against it with `_ITERATOR_DEBUG_LEVEL` 0, so a Debug build of anything that links Slint
# against the debug runtime (/MDd) is 2,032 × `LNK2038` — the audit found the Debug app
# unbuildable, which meant its Debug-only allocation guard had never run in the app. A Debug
# build is therefore /MD with the optimiser off, asserts on and the guard on; it gives up the
# debug heap's checks, which the AddressSanitizer presets cover and more.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

include("${PROJECT_SOURCE_DIR}/cmake/warnings.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/sanitizers.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/deps.cmake")

add_subdirectory(src)

if(TAKT4_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
