add_subdirectory(core)

# takt4-cli: the development console (HANDOFF §8 Phase 1). Core only, built in every
# configuration including UI-less ones, never packaged. Its allocation guard is on in
# Debug so a real-time violation found on the bench aborts rather than hides.
add_executable(takt4-cli cli/main.cpp)
target_link_libraries(takt4-cli PRIVATE takt4::core takt4::warnings)
takt4_enable_rt_guard(takt4-cli DEBUG_ONLY)

if(TAKT4_BUILD_UI)
  add_subdirectory(ui)

  add_executable(takt4 main.cpp)
  target_link_libraries(takt4 PRIVATE takt4::ui takt4::core takt4::warnings)
  takt4_enable_rt_guard(takt4 DEBUG_ONLY)
endif()
