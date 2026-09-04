add_subdirectory(core)

# takt4-cli: the development console (HANDOFF §8 Phase 1). Core only, built in every
# configuration including UI-less ones, never packaged. Its allocation guard is on in
# Debug so a real-time violation found on the bench aborts rather than hides.
add_executable(takt4-cli cli/main.cpp)
target_link_libraries(takt4-cli PRIVATE takt4::core takt4::warnings)
# `beats` needs a weight set and `track` a state space, and the console is a bench tool
# that is never packaged and never relocated, so it defaults to the ones in the source
# tree. The application will resolve them next to itself instead, when Phase 7 gives it
# an installer.
target_compile_definitions(takt4-cli PRIVATE
  "TAKT4_WEIGHTS_DIR=\"${PROJECT_SOURCE_DIR}/assets/weights\""
  "TAKT4_STATESPACE_DIR=\"${PROJECT_SOURCE_DIR}/assets/statespace\""
)
takt4_enable_rt_guard(takt4-cli DEBUG_ONLY)

if(TAKT4_BUILD_UI)
  add_subdirectory(ui)

  add_executable(takt4 main.cpp)
  target_link_libraries(takt4 PRIVATE takt4::ui takt4::core takt4::warnings)
  takt4_enable_rt_guard(takt4 DEBUG_ONLY)

  # The same weight set and state space the console reads, from the same place in the
  # source tree, for the same reason: there is no installer yet. Phase 7 replaces this
  # with paths resolved next to the executable — see HANDOFF §8 Phase 7.
  target_compile_definitions(takt4_ui PRIVATE
    "TAKT4_WEIGHTS_DIR=\"${PROJECT_SOURCE_DIR}/assets/weights\""
    "TAKT4_STATESPACE_DIR=\"${PROJECT_SOURCE_DIR}/assets/statespace\""
  )
endif()
