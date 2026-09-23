add_subdirectory(core)

# takt4-cli: the development console (HANDOFF §8 Phase 1). Core only, built in every
# configuration including UI-less ones, never packaged. Its allocation guard is on in
# Debug so a real-time violation found on the bench aborts rather than hides.
add_executable(takt4-cli cli/annotate.cpp cli/annotate.hpp cli/console.hpp cli/main.cpp)
target_link_libraries(takt4-cli PRIVATE takt4::core takt4::warnings)
if(WIN32)
  # UTF-8 as the process code page, so a WAV named outside the machine's code page opens. See
  # src/utf8.manifest; the linker merges it with the manifest it writes anyway.
  target_sources(takt4-cli PRIVATE utf8.manifest)
endif()
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

  if(WIN32)
    # **A window application, not a console one.** Launched from Explorer, a console-subsystem
    # binary is given a console of its own — so takt4 opened a black cmd box beside its window
    # and left it there for the whole session, filling with whatever Slint felt like logging.
    # Reported from a rig on 2026-09-08.
    #
    # `/ENTRY:mainCRTStartup` keeps `main(argc, argv)` as the entry point, so nothing here has
    # to grow a `WinMain` or a second argument parser; the linker's subsystem is the only thing
    # that changes. `--version` and `--help` still print — `main` attaches to the console it
    # was launched from when there is one. See src/main.cpp.
    set_target_properties(takt4 PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(takt4 PRIVATE "/ENTRY:mainCRTStartup")
    # UTF-8 as the process code page: device and port names read through the ANSI API arrive
    # as UTF-8, which is what the window needs them in. See src/utf8.manifest.
    target_sources(takt4 PRIVATE utf8.manifest)
  endif()

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
