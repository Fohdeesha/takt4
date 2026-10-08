add_subdirectory(core)

# takt4_version_resource(<target> <file name> <description>): the Windows version resource
# from src/version.rc.in — FileVersion the release, ProductVersion the commit (the audit found
# none, which made a crash report from the field harder to place).
function(takt4_version_resource target file description)
  if(NOT WIN32)
    return()
  endif()
  set(TAKT4_RC_NAME "${target}")
  set(TAKT4_RC_FILE "${file}")
  set(TAKT4_RC_DESCRIPTION "${description}")
  set(TAKT4_RC_ICON "${PROJECT_SOURCE_DIR}/assets/icon/takt4.ico")
  set(rc "${PROJECT_BINARY_DIR}/generated/${target}_version.rc")
  configure_file("${PROJECT_SOURCE_DIR}/src/version.rc.in" "${rc}" @ONLY)
  target_sources(${target} PRIVATE "${rc}")
endfunction()

# takt4-cli: the development console (HANDOFF §8 Phase 1). Built in every configuration
# including UI-less ones, and attached to every release beside the app. Its allocation guard
# is on in Debug so a real-time violation found on the bench aborts rather than hides.
add_executable(takt4-cli cli/annotate.cpp cli/annotate.hpp cli/console.hpp cli/main.cpp)
target_link_libraries(takt4-cli PRIVATE takt4::core takt4::warnings)
if(WIN32)
  # UTF-8 as the process code page, so a WAV named outside the machine's code page opens. See
  # src/utf8.manifest; the linker merges it with the manifest it writes anyway.
  target_sources(takt4-cli PRIVATE utf8.manifest)
endif()
# `beats` needs a weight set and `track` a state space. A bare name that is not the built-in
# one is looked up in the source tree, which only a bench has; the built-in set and the
# default state space come from the copies compiled into the binary wherever the assets are
# built (every full build, which is what ships), so the console that goes out with a release
# works on any machine (the audit's H19). A core-only build has no assets and reads the
# source tree for everything.
target_compile_definitions(takt4-cli PRIVATE
  "TAKT4_WEIGHTS_DIR=\"${PROJECT_SOURCE_DIR}/assets/weights\""
  "TAKT4_STATESPACE_DIR=\"${PROJECT_SOURCE_DIR}/assets/statespace\""
)
if(TARGET takt4_assets)
  target_link_libraries(takt4-cli PRIVATE takt4::assets)
  target_compile_definitions(takt4-cli PRIVATE TAKT4_CLI_EMBEDDED_ASSETS=1)
endif()
takt4_enable_rt_guard(takt4-cli DEBUG_ONLY)
takt4_version_resource(takt4-cli takt4-cli.exe "takt4-cli - the takt4 development console")

if(TAKT4_BUILD_UI)
  add_subdirectory(ui)

  add_executable(takt4 main.cpp)
  target_link_libraries(takt4 PRIVATE takt4::ui takt4::core takt4::warnings)
  takt4_enable_rt_guard(takt4 DEBUG_ONLY)
  takt4_version_resource(takt4 takt4.exe "takt4 - beat, downbeat and tempo tracker")

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

  if(APPLE)
    # **An app bundle, takt4.app, not a bare executable**, and not only for the Finder. macOS ends
    # a program that opens an audio input without saying why (NSMicrophoneUsageDescription), and
    # only a bundle's Info.plist can say it; the network sentence is what macOS shows when it asks
    # whether takt4 may reach the local network (Link, OSC, Art-Net). The identifier is what
    # macOS files the operator's answer to both under. Settings are kept beside the bundle, not in
    # it: see `settings::programFolder`.
    set_target_properties(takt4 PROPERTIES
      MACOSX_BUNDLE ON
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
      MACOSX_BUNDLE_BUNDLE_NAME "takt4"
      MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.fohdeesha.takt4"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
      MACOSX_BUNDLE_ICON_FILE "takt4.icns"
    )
    # The Dock's and the Finder's icon, in the bundle's Resources (tools/make_icon.py).
    set(takt4_icns "${PROJECT_SOURCE_DIR}/assets/icon/takt4.icns")
    target_sources(takt4 PRIVATE "${takt4_icns}")
    set_source_files_properties("${takt4_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
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
