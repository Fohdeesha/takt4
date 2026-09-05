# The test binary links takt4_core only — never takt4_ui — so it doubles as the proof
# that the engine stands on its own without Slint.
#
# RTNeural and nlohmann/json are linked here directly for their smoke tests; the engine
# does not use them yet.

add_executable(takt4_tests
  build_info_test.cpp
  audio/channel_picker_test.cpp
  audio/devices_test.cpp
  audio/hop_accumulator_test.cpp
  audio/hop_fanout_test.cpp
  audio/hop_meter_test.cpp
  audio/host_apis_test.cpp
  audio/input_pipeline_test.cpp
  audio/input_stream_test.cpp
  audio/resampler_test.cpp
  dsp/real_fft_test.cpp
  engine/beat_engine_test.cpp
  engine/control_test.cpp
  engine/live_tracker_test.cpp
  features/feature_extractor_test.cpp
  features/filterbank_test.cpp
  io/npy_file_test.cpp
  io/wav_file_test.cpp
  model/activation_engine_test.cpp
  model/beat_model_test.cpp
  model/weights_test.cpp
  output/link_peers_test.cpp
  output/link_session_test.cpp
  output/midi_clock_test.cpp
  output/midi_ports_test.cpp
  output/osc_test.cpp
  rt/alloc_guard_test.cpp
  rt/published_test.cpp
  rt/spsc_ring_test.cpp
  tracking/particle_filter_test.cpp
  tracking/state_space_test.cpp
  tracking/tap_tempo_test.cpp
  tracking/tempo_tracker_test.cpp
  deps/json_smoke_test.cpp
  deps/rtneural_smoke_test.cpp
  support/crt_dialogs.cpp
  support/temp_dir.hpp
)

target_include_directories(takt4_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The golden feature excerpts (tests/data/features, see the README there) and the
# committed blobs (assets/weights, assets/statespace) are read in place from the source
# tree; the test binary is not relocatable, and need not be.
target_compile_definitions(takt4_tests PRIVATE
  "TAKT4_TEST_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/data\""
  "TAKT4_WEIGHTS_DIR=\"${PROJECT_SOURCE_DIR}/assets/weights\""
  "TAKT4_STATESPACE_DIR=\"${PROJECT_SOURCE_DIR}/assets/statespace\""
)

target_link_libraries(takt4_tests
  PRIVATE
    takt4::core
    RTNeural
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
    takt4::warnings
)

# The real-time allocation guard is on in every configuration here: the [rt] tests
# prove the audio-thread code allocates nothing, and would prove nothing without it.
takt4_enable_rt_guard(takt4_tests ALWAYS)

include(Catch)
catch_discover_tests(takt4_tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------------------
# takt4_ui_tests — the window, under Slint's testing backend.
#
# A second binary rather than more files in the first, so that takt4_tests keeps meaning
# "the engine stands up with no Slint anywhere near it" (HANDOFF §4.2). This one drives
# the real component: it presses the real button, steps the real redraw timer and reads
# the real properties back. slint::testing::init() gives it a backend whose windows never
# reach a screen, which is what makes it runnable on a machine with no display — see
# tests/ui/main.cpp.
# ---------------------------------------------------------------------------------------
if(TAKT4_BUILD_UI)
  add_executable(takt4_ui_tests
    ui/main.cpp
    ui/window_test.cpp
    support/crt_dialogs.cpp
  )

  target_include_directories(takt4_ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

  # The committed excerpt is read in place too: the control tests drive the window against
  # a tracker that has really followed it, which is what lets them assert a tempo rather
  # than only that a callback was reached.
  target_compile_definitions(takt4_ui_tests PRIVATE
    "TAKT4_TEST_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/data\""
    "TAKT4_WEIGHTS_DIR=\"${PROJECT_SOURCE_DIR}/assets/weights\""
    "TAKT4_STATESPACE_DIR=\"${PROJECT_SOURCE_DIR}/assets/statespace\""
  )

  target_link_libraries(takt4_ui_tests
    PRIVATE
      takt4::ui
      takt4::core
      Catch2::Catch2
      takt4::warnings
  )

  catch_discover_tests(takt4_ui_tests DISCOVERY_MODE PRE_TEST)
endif()
