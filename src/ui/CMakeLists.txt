# takt4_ui — the Slint front end. Depends on takt4_core; nothing depends on it except
# the executable.

# The Slint compiler's output gets a target of its own: generated code is not clean under
# our warning flags and is not ours to fix, so this target compiles with warnings off
# while the takt4_ui sources keep theirs. (/W0 rather than /w: the Visual Studio
# generator turns /W<n> into the project's warning-level setting, whereas /w is passed
# through next to MSBuild's default /W1 and the compiler reports the clash, D9025, on
# every build.)
add_library(takt4_ui_slint STATIC)
# One entry point, not two. Each generated translation unit embeds the resources its
# window uses, and both windows use the same std-widgets fonts — compiling them separately
# gives a screenful of LNK4006 "already defined; second definition ignored" and two copies
# of every font in the binary. main_window.slint re-exports RulesWindow instead, so there
# is one generated header holding both.
slint_target_sources(takt4_ui_slint main_window.slint)
set_target_properties(takt4_ui_slint PROPERTIES
  # Never let generated code refer to files by their build-machine paths.
  SLINT_EMBED_RESOURCES embed-files
  # The generated header is third-party code too: consumers get its directory as a
  # system include, so our warning flags do not apply to it there either.
  SYSTEM ON
)
if(MSVC)
  target_compile_options(takt4_ui_slint PRIVATE /W0)
else()
  target_compile_options(takt4_ui_slint PRIVATE -w)
endif()
target_link_libraries(takt4_ui_slint PUBLIC Slint::Slint)
# **macOS: Slint's library patched before anything links it.** Skia's prebuilt bindings define
# their own hidden typeinfo for std::exception, which took every `catch (const std::exception&)`
# in takt4 away from takt4's own exceptions on a Mac — see cmake/apple_skia_typeinfo.cmake. Here
# rather than in cmake/deps.cmake, whose every change costs CI a cold Slint build.
if(APPLE)
  add_custom_target(takt4_skia_typeinfo
    COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${CMAKE_BINARY_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/apple_skia_typeinfo.cmake"
    COMMENT "Keeping Skia's copy of the C++ runtime's type information to Skia"
    VERBATIM)
  add_dependencies(takt4_skia_typeinfo cargo-build_slint_cpp)
  add_dependencies(takt4_ui_slint takt4_skia_typeinfo)
endif()

add_library(takt4_ui STATIC
  app.cpp
  app.hpp
  crash_report.cpp
  crash_report.hpp
  delete_guard.cpp
  delete_guard.hpp
  file_dialog.cpp
  file_dialog.hpp
  fixtures_controller.cpp
  fixtures_controller.hpp
  headless.cpp
  headless.hpp
  model_rows.hpp
  native_window.cpp
  native_window.hpp
  rule_presets.cpp
  rule_presets.hpp
  rule_text.cpp
  rule_text.hpp
  rules_controller.cpp
  rules_controller.hpp
  rules_controller_liberation.cpp
  rules_controller_publish.cpp
  rules_controller_slots.cpp
  rules_editor_support.hpp
  shot.cpp
  window_controller.cpp
  window_controller.hpp
  window_state.cpp
  window_state.hpp
)
add_library(takt4::ui ALIAS takt4_ui)

# macOS's open and save panels are Objective-C (file_dialog_mac.mm); file_dialog.cpp holds the
# other systems'.
if(APPLE)
  target_sources(takt4_ui PRIVATE file_dialog_mac.mm) # OBJCXX is enabled in the top directory
  target_link_libraries(takt4_ui PRIVATE "-framework AppKit" "-framework UniformTypeIdentifiers")
endif()

target_include_directories(takt4_ui PUBLIC "${PROJECT_SOURCE_DIR}/src")

# takt4_ui_slint is PUBLIC because window_controller.hpp — a header consumers include —
# includes the generated main_window.h. It arrives as a system include (SYSTEM ON above),
# so our warning flags still do not apply to generated code in a consumer either.
target_link_libraries(takt4_ui
  PUBLIC
    takt4::core
    takt4_ui_slint
  PRIVATE
    # The weights and the state space, compiled in: app.cpp starts the tracker from these
    # rather than from a folder beside the exe. See src/core/CMakeLists.txt.
    takt4::assets
    takt4::warnings
)
