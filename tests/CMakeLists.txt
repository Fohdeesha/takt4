# The test binary links takt4_core only — never takt4_ui — so it doubles as the proof
# that the engine stands on its own without Slint.
#
# RTNeural and nlohmann/json are also linked here directly, for the smoke tests that pin
# their behaviour on their own; the engine uses both (the model and the settings file).

add_executable(takt4_tests
  build_info_test.cpp
  sandbox_test.cpp
  audio/asio_dll_check_test.cpp
  audio/asio_scan_test.cpp
  audio/channel_picker_test.cpp
  cli/console_test.cpp
  audio/devices_test.cpp
  audio/hop_accumulator_test.cpp
  audio/hop_fanout_test.cpp
  audio/hop_meter_test.cpp
  audio/host_apis_test.cpp
  audio/input_pipeline_test.cpp
  audio/input_stream_test.cpp
  audio/input_watchdog_test.cpp
  audio/playback_stream_test.cpp
  audio/resampler_test.cpp
  audio/stream_setup_test.cpp
  control/midi_binding_test.cpp
  control/midi_control_test.cpp
  control/osc_control_test.cpp
  control/osc_parse_test.cpp
  dmx/artnet_live_test.cpp
  dmx/artnet_test.cpp
  dmx/dmx_engine_test.cpp
  dmx/dmx_rule_test.cpp
  dmx/fixture_test.cpp
  dsp/real_fft_test.cpp
  engine/beat_engine_test.cpp
  engine/control_test.cpp
  engine/live_tracker_test.cpp
  features/feature_extractor_test.cpp
  features/filterbank_test.cpp
  features/intensity_test.cpp
  io/blob_file_test.cpp
  io/npy_file_test.cpp
  io/utf8_test.cpp
  io/wav_file_test.cpp
  model/activation_engine_test.cpp
  model/beat_model_test.cpp
  model/weights_test.cpp
  output/beat_scheduler_test.cpp
  output/link_peers_test.cpp
  output/link_session_test.cpp
  output/link_watch_test.cpp
  output/midi_clock_test.cpp
  output/midi_ports_test.cpp
  output/osc_test.cpp
  output/output_runner_test.cpp
  output/routing_test.cpp
  output/rule_sink_test.cpp
  output/transports_test.cpp
  rt/alloc_guard_test.cpp
  settings/rule_json_test.cpp
  settings/settings_test.cpp
  rt/published_test.cpp
  rt/spsc_ring_test.cpp
  rt/thread_priority_test.cpp
  tracking/annotation_test.cpp
  tracking/forward_filter_test.cpp
  tracking/particle_filter_test.cpp
  tracking/state_space_test.cpp
  tracking/tap_tempo_test.cpp
  tracking/tempo_tracker_test.cpp
  trigger/generator_test.cpp
  trigger/rule_test.cpp
  trigger/trigger_engine_test.cpp
  trigger/value_test.cpp
  deps/json_smoke_test.cpp
  deps/rtneural_smoke_test.cpp
  support/crt_dialogs.cpp
  support/loopback_receiver.hpp
  support/rig_sandbox.cpp
  support/scoped_env.hpp
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
    # Directly too, for the test that holds what PortAudio here says of each ASIO device
    # against what the scanning process found (audio/asio_scan_test.cpp).
    portaudio
    RTNeural
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
    takt4::warnings
)

# The real-time allocation guard is on in every configuration here: the [rt] tests
# prove the audio-thread code allocates nothing, and would prove nothing without it.
takt4_enable_rt_guard(takt4_tests ALWAYS)

include(Catch)
# Tags as ctest labels, so the default test presets can leave out what touches the rig —
# [network] (Link, real sockets on well-known ports) and [hardware] (the audio interface and MIDI
# devices themselves). The `-all` presets run everything. See the audit's T2.
#
# And a timeout, so a test that hangs fails instead of holding ctest until somebody notices.
# The slowest test measured was 37 s under ASan with four running at once; 300 s is eight times
# that.
set(TAKT4_TEST_TIMEOUT 300)
# The Skia takt4 draws with is the prebuilt archive it was checked against, library by library
# (cmake/check_skia.cmake). Windows x64 with the UI built, which is what ships.
if(TAKT4_BUILD_UI AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_test(NAME "the prebuilt Skia is the archive takt4 was checked against"
           COMMAND "${CMAKE_COMMAND}" "-DROOT=${PROJECT_BINARY_DIR}"
                   -P "${PROJECT_SOURCE_DIR}/cmake/check_skia.cmake")
  set_tests_properties("the prebuilt Skia is the archive takt4 was checked against"
                       PROPERTIES LABELS "deps" TIMEOUT 300)
endif()

# The console that ships beside the app, run as it is shipped: the built-in weights and the
# default state space have to come from inside it, not from this source tree — which is what
# made the released takt4-cli fail on every other machine (the audit's H19). Only where the
# assets are built, which every full build and every release is.
if(TARGET takt4_assets)
  add_test(NAME "the console that ships reads the weights built into it"
           COMMAND takt4-cli beats "${PROJECT_SOURCE_DIR}/tests/data/features/synthetic.wav")
  set_tests_properties(
    "the console that ships reads the weights built into it"
    PROPERTIES PASS_REGULAR_EXPRESSION "weights electronic \\(built in\\)" LABELS "cli"
               TIMEOUT 300 ENVIRONMENT "TAKT4_NO_ASIO=1;TAKT4_TEST_SANDBOX=1")
  # A script, which holds the exit code and what was tracked as well as the header — see it.
  add_test(NAME "the console that ships tracks with what is built into it"
           COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:takt4-cli>"
                   "-DWAV=${PROJECT_SOURCE_DIR}/tests/data/features/synthetic.wav"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_track_file.cmake")
  set_tests_properties(
    "the console that ships tracks with what is built into it"
    PROPERTIES LABELS "cli" TIMEOUT 300 ENVIRONMENT "TAKT4_NO_ASIO=1;TAKT4_TEST_SANDBOX=1")
endif()

# The console's own parsing (the audit's Low list): an empty value was read as 0, so
# `--device ""` opened device 0; "nan" was a number; and a misspelt option over a file was
# reported as a device option. Each is refused now, in words that name what was wrong.
set(synthetic_wav "${PROJECT_SOURCE_DIR}/tests/data/features/synthetic.wav")
add_test(NAME "the console refuses an empty number rather than reading it as zero"
         COMMAND takt4-cli meter --device "" --channel 1)
add_test(NAME "the console refuses nan as a number"
         COMMAND takt4-cli track "${synthetic_wav}" --latency nan)
add_test(NAME "the console names an option it does not know"
         COMMAND takt4-cli track "${synthetic_wav}" --no-such-option)
set_tests_properties("the console refuses an empty number rather than reading it as zero"
                     PROPERTIES PASS_REGULAR_EXPRESSION "--device: not a number: \"\"")
set_tests_properties("the console refuses nan as a number"
                     PROPERTIES PASS_REGULAR_EXPRESSION "--latency: not a number: \"nan\"")
set_tests_properties("the console names an option it does not know"
                     PROPERTIES PASS_REGULAR_EXPRESSION "unknown option: --no-such-option")
# And annotate's, which had a parser of its own that the fix above never reached (the audit of
# 2026-09-25, L46): `--snap nan` turned snapping off, and a meter was made an int before it was
# checked — 4.5 a bar of four, and 1e10 undefined.
# Each names a taps file that is not there, so a regression stops at reading it — before the
# playback that tapping along would start on this machine's speakers.
set(no_taps "${CMAKE_CURRENT_BINARY_DIR}/no-such.taps")
add_test(NAME "annotate refuses nan as a number"
         COMMAND takt4-cli annotate "${synthetic_wav}" --taps "${no_taps}" --snap nan)
add_test(NAME "annotate refuses a meter that is not a whole number of beats"
         COMMAND takt4-cli annotate "${synthetic_wav}" --taps "${no_taps}" --meter 4.5)
set_tests_properties("annotate refuses nan as a number"
                     PROPERTIES PASS_REGULAR_EXPRESSION "--snap: expected a number, got 'nan'")
set_tests_properties("annotate refuses a meter that is not a whole number of beats"
                     PROPERTIES PASS_REGULAR_EXPRESSION "--meter expects a whole number from 1 to 16")
# No ASIO and the sandbox, as every test binary has (tests/support/crt_dialogs.cpp): a parsing
# regression in the first would otherwise open device 0 — with every ASIO driver loaded — for
# as long as the timeout (the audit of 2026-09-25, T4).
set_tests_properties("the console refuses an empty number rather than reading it as zero"
                     "the console refuses nan as a number"
                     "the console names an option it does not know"
                     "annotate refuses nan as a number"
                     "annotate refuses a meter that is not a whole number of beats"
                     PROPERTIES LABELS "cli" TIMEOUT 60
                                ENVIRONMENT "TAKT4_NO_ASIO=1;TAKT4_TEST_SANDBOX=1")

# The console's live path, on the default input — see the script. ASIO kept out of it, as the
# default presets keep it out of every test: the MOTU's driver belongs to the rig.
add_test(NAME "the console tracks a live input through the application's own chain"
         COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:takt4-cli>"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_track_device.cmake")
set_tests_properties("the console tracks a live input through the application's own chain"
                     PROPERTIES LABELS "cli;hardware" TIMEOUT 120 ENVIRONMENT "TAKT4_NO_ASIO=1"
                                SKIP_REGULAR_EXPRESSION "SKIP")

if(WIN32)
  # GetFileVersionInfoW, for the test that reads the executables' version resources.
  target_link_libraries(takt4_tests PRIVATE version)
  # UTF-8 as the process code page, as takt4 and takt4-cli have it (src/utf8.manifest): tests
  # of the code that reads names through the ANSI API have to run as the app runs, and a test
  # name with a character outside the old code page otherwise matches no filter at all (the
  # audit of 2026-09-25, B7).
  target_sources(takt4_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/utf8.manifest")
endif()

catch_discover_tests(takt4_tests DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS
                     PROPERTIES TIMEOUT ${TAKT4_TEST_TIMEOUT})

# ---------------------------------------------------------------------------------------
# takt4_ui_tests — the window, under Slint's testing backend.
#
# A second binary rather than more files in the first, so that takt4_tests keeps meaning
# "the engine stands up with no Slint anywhere near it" (HANDOFF §4.2). This one drives
# the real component: it presses the real button, steps the real redraw timer and reads
# the real properties back. takt4's own headless platform (`ui::installHeadlessPlatform`,
# the one takt4-shot renders through) gives it windows that never reach a screen, which is
# what makes it runnable on a machine with no display — see tests/ui/main.cpp for why it is
# not slint::testing::init().
# ---------------------------------------------------------------------------------------
if(TAKT4_BUILD_UI)
  add_executable(takt4_ui_tests
    ui/crash_report_test.cpp
    ui/fixtures_test.cpp
    ui/main.cpp
    ui/model_watch.hpp
    ui/nothing_real.hpp
    ui/rules_test.cpp
    ui/shot.hpp
    ui/window_test.cpp
    support/crt_dialogs.cpp
    support/rig_sandbox.cpp
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
  if(WIN32)
    # As takt4 has it — see takt4_tests above.
    target_sources(takt4_ui_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/utf8.manifest")
  endif()

  catch_discover_tests(takt4_ui_tests DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS
                       PROPERTIES TIMEOUT ${TAKT4_TEST_TIMEOUT})
endif()
