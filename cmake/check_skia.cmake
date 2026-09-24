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

# skia-bindings 0.153.3, which Slint 1.18.1 uses, recorded 2026-09-24. No textlayout since
# Slint 1.18, so the archive holds two libraries where 0.99.0's held six.
set(expected_key "b7f043e0b1e2a850e702-x86_64-pc-windows-msvc-d3d-ganesh-gl-jpegd-jpege-pdf")
set(expected_tag "0.153.3")
set(expected_hashes
  "skia-bindings.lib=d1aed821c5e83e7b658ea1cea7cd90f50bd367abf5253d11b3d25bcc2752da13"
  "skia.lib=639cdebf3f049e2f1aa615746c1e99c04bc8914c80fe29e6ea2a0480c37bb75d")

file(GLOB_RECURSE keys "${ROOT}/key.txt")
set(checked 0)
set(stale "")
foreach(key_file IN LISTS keys)
  get_filename_component(dir "${key_file}" DIRECTORY)
  if(NOT dir MATCHES "skia-bindings-[0-9a-f]+/out/skia$")
    continue()
  endif()
  file(READ "${key_file}" key)
  string(STRIP "${key}" key)
  file(READ "${dir}/tag.txt" tag)
  string(STRIP "${tag}" tag)
  # Another version's copy is what cargo left behind from before a bump: it keeps every build
  # script's output directory, and nothing links the old ones. Said, and not checked. A bump
  # with no hashes recorded still fails below, because then no copy is the version expected.
  if(NOT key STREQUAL expected_key OR NOT tag STREQUAL expected_tag)
    list(APPEND stale "${tag}")
    continue()
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
  set(only "")
  if(stale)
    set(only " (only leftovers of skia-bindings ${stale})")
  endif()
  message(FATAL_ERROR "no unpacked Skia archive ${expected_tag} ${expected_key} under ${ROOT}"
                      "${only}: has Slint been built, or has skia-bindings moved without its "
                      "hashes being recorded here?")
endif()
if(stale)
  list(REMOVE_DUPLICATES stale)
  message(STATUS "left alone: cargo's leftover unpacked archive(s) of skia-bindings ${stale}")
endif()
message(STATUS "Skia prebuilt ${expected_tag} ${expected_key}: ${checked} unpacked cop(ies), every library as checked")
