# The test binary links takt4_core only — never takt4_ui — so it doubles as the proof
# that the engine stands on its own without Slint.
#
# RTNeural and nlohmann/json are linked here directly for their smoke tests; the engine
# does not use them yet.

add_executable(takt4_tests
  build_info_test.cpp
  audio/host_apis_test.cpp
  output/link_session_test.cpp
  output/midi_ports_test.cpp
  deps/json_smoke_test.cpp
  deps/rtneural_smoke_test.cpp
)

target_link_libraries(takt4_tests
  PRIVATE
    takt4::core
    RTNeural
    nlohmann_json::nlohmann_json
    Catch2::Catch2WithMain
    takt4::warnings
)

include(Catch)
catch_discover_tests(takt4_tests DISCOVERY_MODE PRE_TEST)
