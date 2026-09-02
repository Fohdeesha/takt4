cmake_minimum_required(VERSION 3.28)

project(takt4
  VERSION 0.1.0
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

include("${PROJECT_SOURCE_DIR}/cmake/warnings.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/deps.cmake")

add_subdirectory(src)

if(TAKT4_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
