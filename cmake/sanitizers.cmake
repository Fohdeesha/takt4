# takt4 sanitizers — a build of the whole tree with the runtime checks turned on.
#
# Set `TAKT4_SANITIZE` at configure time; the presets `windows-asan`, `linux-asan` and
# `linux-tsan` do it for you. The suite is meant to pass clean under every one of them, and
# `ctest` is how you find out: the sanitizers report at *run* time, so a build that compiles
# has proved nothing at all.
#
# **What each one is for.**
#
#   address      Reads and writes of memory that has been freed, walked off the end of, or
#                never allocated. This is the one that catches the 2026-09-16 crash class —
#                an element destroyed while something still points at it. Also the cheapest
#                to run: roughly 2x slower and a few times the memory.
#   undefined    Signed overflow, bad shifts, null this, misaligned loads, a value that does
#                not fit the enum it is being stored in. Clang and GCC only.
#   thread       Races between the audio thread, the output thread and the UI thread. Clang
#                and GCC only, and it must be the *only* sanitizer in the build.
#
# **Windows has AddressSanitizer and nothing else.** MSVC ships `/fsanitize=address`; it has
# no ThreadSanitizer and no UndefinedBehaviorSanitizer, and neither does clang-cl, because
# neither runtime has a Windows port. So on this machine `windows-asan` is the whole of what
# can be run locally and the other two are run on Linux, where full.yml's linux-tsan job runs.

set(TAKT4_SANITIZE "off" CACHE STRING
  "Runtime checks to build with: off, address, undefined, address+undefined, thread")
set_property(CACHE TAKT4_SANITIZE PROPERTY STRINGS
  off address undefined address+undefined thread)

if(TAKT4_SANITIZE STREQUAL "off")
  return()
endif()

# GCC warns, as an error here, that ThreadSanitizer does not model std::atomic_thread_fence —
# and Kohlhoff asio, which Link bundles, uses fences in its executors, inlined into takt4's own
# link_session.cpp (the second linux-tsan run, 2026-09-23). A fence is TSan's blind spot, not a
# race; takt4's own code has none (rt::Published was changed to do without). Off for GCC's TSan
# build only.
if(TAKT4_SANITIZE STREQUAL "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-Wno-tsan)
endif()

if(MSVC)
  if(NOT TAKT4_SANITIZE STREQUAL "address")
    message(FATAL_ERROR
      "TAKT4_SANITIZE=${TAKT4_SANITIZE} is not available with MSVC: Windows has "
      "AddressSanitizer only. Use TAKT4_SANITIZE=address here and run 'thread' and "
      "'undefined' on the linux preset.")
  endif()

  # `/Zi` so a report names lines rather than addresses — the point of the exercise.
  add_compile_options(/fsanitize=address /Zi)

  # **The container annotations have to come off, because half the binary is not ours.**
  #
  # `/fsanitize=address` makes the MSVC STL annotate the unused capacity of `std::string` and
  # `std::vector` so ASan can catch a read past `size()` inside the allocation. It is a
  # per-object setting and the linker refuses to mix: Slint ships Skia, ICU, harfbuzz and
  # spirv-cross as C++ built inside its cargo build, with no ASan and so no annotations, and
  # linking `takt4_ui_tests` against them gave 1600 x `LNK2038 mismatch detected for
  # 'annotate_string'` (measured 2026-09-16). Nothing here can instrument those.
  #
  # What this costs is container-overflow detection on those two types **and nothing else**:
  # use-after-free, double free, heap and stack buffer overflow, stack-use-after-return and
  # use-after-scope are all untouched — and use-after-free is the class this was turned on
  # for. The Linux presets keep the annotations, because libstdc++ leaves them off by default
  # and there is nothing to disagree with.
  add_compile_definitions(_DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
  # ASan cannot be linked incrementally, and `/DEBUG` is what puts the PDB beside the exe.
  add_link_options(/INCREMENTAL:NO /DEBUG)

  # `/RTC1` is in CMake's default Debug flags and the two are mutually exclusive — the
  # compiler refuses the pair outright. Whole-program optimisation is likewise out. Set
  # plainly rather than with PARENT_SCOPE: `include()` shares the caller's scope, so these
  # land in the top-level one, which is where the generator reads them from.
  foreach(config DEBUG RELWITHDEBINFO)
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_${config} "${CMAKE_CXX_FLAGS_${config}}")
  endforeach()
  string(REPLACE "/GL" "" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

  # The tests have to be able to *run*. MSVC links AddressSanitizer as a DLL, the loader will
  # not find it on PATH, and Visual Studio only adds it when launching from the IDE — so a
  # `ctest` run would fail to start every binary with a missing-DLL box. Copied beside them
  # instead, because `ctest --preset windows-asan` has to work with no setup.
  get_filename_component(_takt4_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(_takt4_asan_dll "${_takt4_msvc_bin}/clang_rt.asan_dynamic-x86_64.dll")
  if(EXISTS "${_takt4_asan_dll}")
    foreach(config Debug Release RelWithDebInfo)
      file(COPY "${_takt4_asan_dll}" DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
    endforeach()
    message(STATUS "takt4: AddressSanitizer on; its runtime copied beside the binaries")
  else()
    message(WARNING
      "takt4: AddressSanitizer on, but clang_rt.asan_dynamic-x86_64.dll is not beside cl.exe "
      "(${_takt4_msvc_bin}). Put it on PATH or the tests will not start.")
  endif()
  return()
endif()

set(_takt4_san_flags "")
if(TAKT4_SANITIZE STREQUAL "address")
  set(_takt4_san_flags -fsanitize=address)
elseif(TAKT4_SANITIZE STREQUAL "undefined")
  set(_takt4_san_flags -fsanitize=undefined -fno-sanitize-recover=all)
elseif(TAKT4_SANITIZE STREQUAL "address+undefined")
  set(_takt4_san_flags -fsanitize=address,undefined -fno-sanitize-recover=all)
elseif(TAKT4_SANITIZE STREQUAL "thread")
  set(_takt4_san_flags -fsanitize=thread)
else()
  message(FATAL_ERROR "TAKT4_SANITIZE=${TAKT4_SANITIZE} is not one of: "
                      "off address undefined address+undefined thread")
endif()

# `-g` for line numbers and `-fno-omit-frame-pointer` so the stack in a report is the stack
# that was actually running; without it the middle of every trace is missing.
add_compile_options(${_takt4_san_flags} -g -fno-omit-frame-pointer)
add_link_options(${_takt4_san_flags})
message(STATUS "takt4: sanitizers on — ${TAKT4_SANITIZE}")
