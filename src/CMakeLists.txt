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

  # takt4-shot: renders the window to an image with no display involved (src/ui/shot.cpp).
  # A bench tool like takt4-cli, never packaged.
  add_executable(takt4-shot shot_main.cpp)
  target_link_libraries(takt4-shot PRIVATE takt4::ui takt4::core takt4::warnings)

  # These three are **takt4-shot's**, not the application's. The app reads its weights and
  # state space out of itself now (takt4::assets, src/core/CMakeLists.txt), which is what
  # makes takt4.exe a file that can be copied anywhere and still start. The shot tool is a
  # bench tool that is never packaged and never moved, and it wants the source tree: it
  # fills the readouts by running the tracker over the committed synthetic excerpt, so the
  # picture holds numbers the engine really made.
  target_compile_definitions(takt4_ui PRIVATE
    "TAKT4_WEIGHTS_DIR=\"${PROJECT_SOURCE_DIR}/assets/weights\""
    "TAKT4_STATESPACE_DIR=\"${PROJECT_SOURCE_DIR}/assets/statespace\""
    "TAKT4_TEST_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/data\""
  )
endif()
