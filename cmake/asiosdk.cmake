# Steinberg ASIO SDK, vendored in third_party/asiosdk and used under its GPLv3 option.
#
# Defines ASIO::host in the same shape PortAudio's own FindASIO.cmake produces. Because
# the target already exists when PortAudio configures, PortAudio's
# `if(PA_USE_ASIO AND TARGET ASIO::host)` branch uses the vendored SDK and never reaches
# its fallback of downloading the SDK from steinberg.net at configure time.
#
# host/pc/asiolist.cpp is patched — see the replacements below — for an array allocated with
# new[] freed with plain delete (PortAudio patches that line too), a driver name copied unbounded
# into a 128-byte field, and a missing-DLL check that could never fail and, once it could, asked
# a question OpenFile cannot answer for a long or unexpanded path.
# The patched copy is written to the build tree so the vendored SDK stays byte-identical to
# Steinberg's package.

if(NOT WIN32)
  message(FATAL_ERROR "cmake/asiosdk.cmake is Windows-only")
endif()

set(asiosdk_root "${PROJECT_SOURCE_DIR}/third_party/asiosdk")
set(asiosdk_gen "${PROJECT_BINARY_DIR}/generated/asiosdk")

foreach(required IN ITEMS
  common/asio.h
  common/iasiodrv.h
  common/asio.cpp
  host/asiodrivers.cpp
  host/pc/asiolist.cpp
)
  if(NOT EXISTS "${asiosdk_root}/${required}")
    message(FATAL_ERROR "third_party/asiosdk/${required} is missing")
  endif()
endforeach()

set(asiolist_in "${asiosdk_root}/host/pc/asiolist.cpp")
set(asiolist_out "${asiosdk_gen}/patched_asiolist.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asiolist_in}")

file(READ "${asiolist_in}" asiolist_source)
set(asiolist_patched "${asiolist_source}")

# Each edit is an exact-text replacement that stops the configure if the text is not there,
# so a changed SDK is noticed instead of silently compiled unpatched.
function(takt4_patch_asiolist what from to)
  string(FIND "${asiolist_patched}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR
      "${asiolist_in} no longer contains the text for \"${what}\". Check whether the SDK "
      "fixed it upstream and update cmake/asiosdk.cmake.")
  endif()
  string(REPLACE "${from}" "${to}" patched "${asiolist_patched}")
  set(asiolist_patched "${patched}" PARENT_SCOPE)
endfunction()

takt4_patch_asiolist("new[] freed with delete" "delete lpdrv" "delete[] lpdrv")

# The driver's description, up to 255 bytes from the registry, copied into a 128-byte name
# with strcpy — a heap overflow for any driver that describes itself at length, at every
# launch, before a window exists (the audit's ASIO section). Bounded, and the source
# terminated first, since RegQueryValueEx does not promise to.
takt4_patch_asiolist("the driver description copy"
  "strcpy(lpdrv->drvname,databuf);"
  "databuf[sizeof(databuf) - 1] = 0; strncpy(lpdrv->drvname,databuf,sizeof(lpdrv->drvname) - 1); lpdrv->drvname[sizeof(lpdrv->drvname) - 1] = 0; /* takt4: bounded */")
takt4_patch_asiolist("the key name copy"
  "else strcpy(lpdrv->drvname,keyname);"
  "else { strncpy(lpdrv->drvname,keyname,sizeof(lpdrv->drvname) - 1); lpdrv->drvname[sizeof(lpdrv->drvname) - 1] = 0; } /* takt4: bounded */")

# OpenFile reports a missing file as HFILE_ERROR, which is -1 and so true: the check that a
# driver's DLL exists could never fail, and every registered driver was loaded at every launch
# — including the two on this machine whose DLLs are gone. A faulty driver can take a process
# down at load with a fault nothing can catch (PortAudio #960, #1148), so one whose DLL is not
# even there is not tried.
takt4_patch_asiolist("the missing-DLL check" "if (hfile) rc = 0;" "if (hfile != HFILE_ERROR) rc = 0; /* takt4 */")
# And the question itself is asked of Windows rather than of OpenFile, which cannot answer it for
# a path of 127 bytes or more, nor for the `%SystemRoot%\...` a REG_EXPAND_SZ holds: read the
# right way round, OpenFile hid drivers that work (the audit of 2026-09-25, M9). See
# src/core/audio/asio_dll_check.hpp, which tests/audio/asio_dll_check_test.cpp holds to it.
takt4_patch_asiolist("the missing-DLL check's question"
  "hfile = OpenFile(dllpath,&ofs,OF_EXIST);"
  "hfile = takt4::audio::asioDriverDllPresent(dllpath, (std::size_t)dllpathsize) ? 1 : HFILE_ERROR; /* takt4 */")
takt4_patch_asiolist("the missing-DLL check's header"
  "#include \"asiolist.h\""
  "#include \"asiolist.h\"\n#include \"${PROJECT_SOURCE_DIR}/src/core/audio/asio_dll_check.hpp\" /* takt4 */")

# Only rewrite when the content changes, so a reconfigure doesn't force a rebuild.
set(asiolist_existing "")
if(EXISTS "${asiolist_out}")
  file(READ "${asiolist_out}" asiolist_existing)
endif()
if(NOT asiolist_existing STREQUAL asiolist_patched)
  file(WRITE "${asiolist_out}" "${asiolist_patched}")
endif()

add_library(ASIO::host INTERFACE IMPORTED)
target_sources(ASIO::host INTERFACE
  "${asiosdk_root}/common/asio.cpp"
  "${asiosdk_root}/host/asiodrivers.cpp"
  "${asiolist_out}"
)
target_include_directories(ASIO::host SYSTEM INTERFACE
  "${asiosdk_root}/common"
  "${asiosdk_root}/host"
  "${asiosdk_root}/host/pc"
)
target_link_libraries(ASIO::host INTERFACE ole32 uuid)
