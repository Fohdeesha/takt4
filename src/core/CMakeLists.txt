# takt4_core — the engine. Must never depend on Slint or anything else UI-related;
# tests and headless builds link this on its own.

configure_file(build_config.hpp.in "${PROJECT_BINARY_DIR}/generated/core/build_config.hpp" @ONLY)

add_library(takt4_core STATIC
  build_info.cpp
  build_info.hpp
  audio/host_apis.cpp
  audio/host_apis.hpp
  audio/portaudio_session.cpp
  audio/portaudio_session.hpp
  output/link_session.cpp
  output/link_session.hpp
  output/midi_ports.cpp
  output/midi_ports.hpp
)
add_library(takt4::core ALIAS takt4_core)

target_include_directories(takt4_core
  PUBLIC
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_BINARY_DIR}/generated"
)

target_link_libraries(takt4_core
  PRIVATE
    portaudio
    Ableton::Link
    rtmidi
    nlohmann_json::nlohmann_json
    takt4::warnings
)

if(WIN32)
  target_compile_definitions(takt4_core
    PUBLIC
      _WIN32_WINNT=0x0A00   # Windows 10; Kohlhoff asio insists on this being explicit
      NOMINMAX
      WIN32_LEAN_AND_MEAN
  )
endif()
