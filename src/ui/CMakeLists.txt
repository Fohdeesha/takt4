# takt4_ui — the Slint front end. Depends on takt4_core; nothing depends on it except
# the executable.

# The Slint compiler's output gets a target of its own: generated code is not clean under
# our warning flags and is not ours to fix, so this target compiles with warnings off
# while the takt4_ui sources keep theirs. (/W0 rather than /w: the Visual Studio
# generator turns /W<n> into the project's warning-level setting, whereas /w is passed
# through next to MSBuild's default /W1 and the compiler reports the clash, D9025, on
# every build.)
add_library(takt4_ui_slint STATIC)
slint_target_sources(takt4_ui_slint main_window.slint)
# Never let generated code refer to files by their build-machine paths.
set_target_properties(takt4_ui_slint PROPERTIES SLINT_EMBED_RESOURCES embed-files)
if(MSVC)
  target_compile_options(takt4_ui_slint PRIVATE /W0)
else()
  target_compile_options(takt4_ui_slint PRIVATE -w)
endif()
target_link_libraries(takt4_ui_slint PUBLIC Slint::Slint)

add_library(takt4_ui STATIC
  app.cpp
  app.hpp
)
add_library(takt4::ui ALIAS takt4_ui)

target_include_directories(takt4_ui PUBLIC "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(takt4_ui
  PUBLIC
    takt4::core
  PRIVATE
    takt4_ui_slint
    takt4::warnings
)
