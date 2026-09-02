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
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(takt4_warnings INTERFACE
      -Wduplicated-cond
      -Wduplicated-branches
      -Wlogical-op
    )
  endif()
  if(TAKT4_WARNINGS_AS_ERRORS)
    target_compile_options(takt4_warnings INTERFACE -Werror)
  endif()
endif()
