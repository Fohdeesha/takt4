# Checks that the Skia libraries in the build are the ones takt4 was checked against.
#
# Slint draws with Skia, and rust-skia's build script downloads Skia prebuilt, over HTTPS but
# with no checksum of its own (the audit: "the skia-bindings download is unchecked"). So the
# archive it unpacked is compared, library by library, with the sha256 recorded here, and a
# difference fails. Run as a test (tests/CMakeLists.txt), because the archive only exists once
# cargo has built Slint, and globbing the build tree on every build would slow every build.
#
# Windows x64 only: that is the platform takt4 ships on, and the one these were recorded for.
# When skia-bindings moves, record the new archive's hashes here in the same commit.
#
#   cmake -DROOT=<build tree> -P cmake/check_skia.cmake

set(expected_key "a25a0fdb7d90429aa2d1-x86_64-pc-windows-msvc-d3d-gl-jpegd-jpege-pdf-textlayout")
set(expected_tag "0.99.0")
set(expected_hashes
  "skia-bindings.lib=9633c9646c6e780e23ca5c994795890822c11b95f80a0ea99589dd2fc2bf4930"
  "skia.lib=2e58b336bc6f5c57668bad59d782c416371fb0a75c853d7f85cc3865ad814853"
  "skparagraph.lib=6cd4f80073e0a0000fafdf2878478edb6850bdd80a6459a028759fdc2aa9e480"
  "skshaper.lib=75083b2117b73a4975f01c816f31e15926f862327bbaf2f4d3b9e7625e66959a"
  "skunicode_core.lib=12c4c5531f21860474fce4f00493a7b926bdd59c810477ea85ae81c77946aa0c"
  "skunicode_icu.lib=7b24cd096cd4702d098c623015ab09e1d99df497c3dbdd34e9fa51ffd32a1953")

file(GLOB_RECURSE keys "${ROOT}/key.txt")
set(checked 0)
foreach(key_file IN LISTS keys)
  get_filename_component(dir "${key_file}" DIRECTORY)
  if(NOT dir MATCHES "skia-bindings-[0-9a-f]+/out/skia$")
    continue()
  endif()
  file(READ "${key_file}" key)
  string(STRIP "${key}" key)
  file(READ "${dir}/tag.txt" tag)
  string(STRIP "${tag}" tag)
  if(NOT key STREQUAL expected_key OR NOT tag STREQUAL expected_tag)
    message(FATAL_ERROR "${dir}: Skia archive ${tag} ${key}, but ${expected_tag} ${expected_key} "
                        "is what was checked")
  endif()
  foreach(pair IN LISTS expected_hashes)
    string(REPLACE "=" ";" parts "${pair}")
    list(GET parts 0 name)
    list(GET parts 1 want)
    file(SHA256 "${dir}/${name}" have)
    if(NOT have STREQUAL want)
      message(FATAL_ERROR "${dir}/${name} has sha256 ${have}, but ${want} is what was checked")
    endif()
  endforeach()
  math(EXPR checked "${checked} + 1")
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "no unpacked Skia archive under ${ROOT}: has Slint been built?")
endif()
message(STATUS "Skia prebuilt ${expected_tag} ${expected_key}: ${checked} unpacked cop(ies), every library as checked")
