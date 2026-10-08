# takt4::warnings — compiler settings for takt4's own targets only.
#
# Third-party code is built with its own flags. Third-party headers are pulled in as
# SYSTEM includes (see deps.cmake), so the strict warnings below only fire on our code.

add_library(takt4_warnings INTERFACE)
add_library(takt4::warnings ALIAS takt4_warnings)

if(MSVC)
  target_compile_options(takt4_warnings INTERFACE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
    /Zc:preprocessor
    # Headers included with <angle brackets> are treated as external: no warnings.
    /external:anglebrackets
    /external:W0
  )
  if(TAKT4_WARNINGS_AS_ERRORS)
    target_compile_options(takt4_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(takt4_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    # Not -Wnull-dereference: it is an optimiser pass, not a front-end check, so the
    # SYSTEM include marking does not silence it. GCC 13 at -O3 reports it inside asio's
    # any_executor equality (io_context.hpp, via ableton/Link.hpp) when compiling
    # link_session.cpp, and there is nothing of ours to fix.
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(takt4_warnings INTERFACE
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
    )
    # GCC 12, which the Linux release is built with (Ubuntu 22.04, for its older glibc), reports
    # a std::string built with + as a memcpy that may overlap or overflow, from inside
    # libstdc++'s own char_traits::copy at -O3: rule_text.cpp, rules_controller_slots.cpp and
    # three tests, all plain concatenation (the first Linux release build, 2026-10-08); and, the
    # same day, -Warray-bounds on a vector of std::arrays copied in feature_extractor_test.cpp,
    # inside the same library's memcpy. GCC 13 builds all of it clean (linux-tsan). Those three
    # warnings, on GCC 12 and older only.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
      target_compile_options(takt4_warnings INTERFACE
        -Wno-restrict -Wno-stringop-overflow -Wno-array-bounds)
    endif()
  endif()
  if(TAKT4_WARNINGS_AS_ERRORS)
    target_compile_options(takt4_warnings INTERFACE -Werror)
  endif()
endif()
